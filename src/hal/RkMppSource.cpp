#include "msad/RkMppSource.h"
#include "msad/Logger.h"
#include <chrono>
#include <thread>
#include <atomic>
#include <stdexcept>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/bsf.h>
#include <rk_mpi.h>
#include <mpp_buffer.h>
}
namespace msad {
namespace {
int64_t clockMs(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
}
struct RkMppSource::State {
    AVFormatContext* input=nullptr;
    AVBSFContext* filter=nullptr;
    AVPacket* packet=av_packet_alloc();
    MppCtx decoder=nullptr;
    MppApi* api=nullptr;
    MppPacket pending=nullptr;
    MppBufferGroup group=nullptr;
    int stream=-1;
    int64_t deadline=0;
    std::shared_ptr<std::atomic<bool>> stopped;
    bool eof=false,eosSent=false,network=false,complete=false,produced=false;
    ~State(){
        if(pending)mpp_packet_deinit(&pending);
        if(decoder){if(api)api->reset(decoder);mpp_destroy(decoder);}
        if(group)mpp_buffer_group_put(group);
        av_packet_free(&packet);av_bsf_free(&filter);avformat_close_input(&input);
    }
    static int interrupt(void* opaque){auto* s=static_cast<State*>(opaque);return s->stopped->load()||clockMs()>s->deadline;}
};
RkMppSource::RkMppSource()=default;
RkMppSource::~RkMppSource(){close();}
void RkMppSource::close(){state_.reset();}
bool RkMppSource::open(const std::string& uri){
    close();uri_=uri;
    if(stopped_->load())return false;
    auto s=std::make_shared<State>();
    s->stopped=stopped_;
    s->network=uri.rfind("rtsp://",0)==0||uri.rfind("rtsps://",0)==0;
    s->input=avformat_alloc_context();
    if(!s->input||!s->packet)return false;
    s->deadline=clockMs()+8000;
    s->input->interrupt_callback={State::interrupt,s.get()};
    AVDictionary* options=nullptr;
    if(s->network)av_dict_set(&options,"rtsp_transport","tcp",0);
    const int opened=avformat_open_input(&s->input,uri.c_str(),nullptr,&options);
    av_dict_free(&options);
    if(opened<0||avformat_find_stream_info(s->input,nullptr)<0)return false;
    s->stream=av_find_best_stream(s->input,AVMEDIA_TYPE_VIDEO,-1,-1,nullptr,0);
    if(s->stream<0)return false;
    auto* par=s->input->streams[s->stream]->codecpar;
    const bool h264=par->codec_id==AV_CODEC_ID_H264;
    if(!h264&&par->codec_id!=AV_CODEC_ID_HEVC){Logger::error("MPP","only H264/H265 supported");return false;}
    if(par->width<2||par->height<2||par->width>1920||par->height>1088){Logger::error("MPP","source must be within 1920x1088 decode budget");return false;}
    const auto* bsf=av_bsf_get_by_name(h264?"h264_mp4toannexb":"hevc_mp4toannexb");
    if(!bsf||av_bsf_alloc(bsf,&s->filter)<0||avcodec_parameters_copy(s->filter->par_in,par)<0)return false;
    s->filter->time_base_in=s->input->streams[s->stream]->time_base;
    if(av_bsf_init(s->filter)<0||mpp_create(&s->decoder,&s->api)!=MPP_OK)return false;
    RK_U32 split=1;
    if(s->api->control(s->decoder,MPP_DEC_SET_PARSER_SPLIT_MODE,&split)!=MPP_OK||
       mpp_init(s->decoder,MPP_CTX_DEC,h264?MPP_VIDEO_CodingAVC:MPP_VIDEO_CodingHEVC)!=MPP_OK)return false;
    state_=std::move(s);
    Logger::info("MPP",std::string("hardware decoder ready: ")+(h264?"H264":"H265")+(state_->network?" RTSP/TCP":" looping file"));
    return true;
}
bool RkMppSource::readFrame(Frame& out){
    if(!state_||stopped_->load())return false;
    if(state_->complete){if(state_->network||!state_->produced)return false;return open(uri_)&&readFrame(out);}
    auto s=state_;s->deadline=clockMs()+8000;
    while(!stopped_->load()&&clockMs()<s->deadline){
        MppFrame decoded=nullptr;
        const auto result=s->api->decode_get_frame(s->decoder,&decoded);
        if(result!=MPP_OK&&result!=MPP_ERR_TIMEOUT)return false;
        if(decoded){
            if(mpp_frame_get_info_change(decoded)){
                const auto bytes=mpp_frame_get_buf_size(decoded);
                if(bytes>4U*1024*1024||mpp_frame_get_width(decoded)>1920||mpp_frame_get_height(decoded)>1088){mpp_frame_deinit(&decoded);return false;}
                if(!s->group&&mpp_buffer_group_get_internal(&s->group,MPP_BUFFER_TYPE_DRM)!=MPP_OK){mpp_frame_deinit(&decoded);return false;}
                // Bounded decoder pool; callers must not retain more surfaces than its capacity.
                auto rc=mpp_buffer_group_limit_config(s->group,bytes,24);
                if(rc==MPP_OK)rc=s->api->control(s->decoder,MPP_DEC_SET_EXT_BUF_GROUP,s->group);
                if(rc==MPP_OK)rc=s->api->control(s->decoder,MPP_DEC_SET_INFO_CHANGE_READY,nullptr);
                mpp_frame_deinit(&decoded);if(rc!=MPP_OK)return false;continue;
            }
            const bool eos=mpp_frame_get_eos(decoded)!=0;
            auto buffer=mpp_frame_get_buffer(decoded);
            if(buffer&&!mpp_frame_get_errinfo(decoded)&&!mpp_frame_get_discard(decoded)){
                if(mpp_frame_get_fmt(decoded)!=MPP_FMT_YUV420SP){mpp_frame_deinit(&decoded);return false;}
                out=Frame{};out.width=static_cast<int>(mpp_frame_get_width(decoded));out.height=static_cast<int>(mpp_frame_get_height(decoded));
                out.widthStride=static_cast<int>(mpp_frame_get_hor_stride(decoded));out.heightStride=static_cast<int>(mpp_frame_get_ver_stride(decoded));
                out.pixelFormat=Frame::PixelFormat::NV12;out.dmaFd=mpp_buffer_get_fd(buffer);
                out.bufferOwner=std::shared_ptr<void>(decoded,[s](void* ptr){auto frame=static_cast<MppFrame>(ptr);mpp_frame_deinit(&frame);});
                out.frameIndex=++sequence_;out.timestampMs=static_cast<uint64_t>(clockMs());
                s->produced=true;s->complete=eos;
                return true;
            }
            mpp_frame_deinit(&decoded);
            if(eos){if(s->network||!s->produced)return false;return open(uri_)&&readFrame(out);}
        }
        if(s->pending){
            if(s->api->decode_put_packet(s->decoder,s->pending)==MPP_OK){mpp_packet_deinit(&s->pending);av_packet_unref(s->packet);}
            else std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if(s->eosSent){std::this_thread::sleep_for(std::chrono::milliseconds(1));continue;}
        int got=av_bsf_receive_packet(s->filter,s->packet);
        if(got==0){
            if(mpp_packet_init(&s->pending,s->packet->data,static_cast<size_t>(s->packet->size))!=MPP_OK)return false;
            mpp_packet_set_pts(s->pending,s->packet->pts);continue;
        }
        if(got==AVERROR_EOF){
            if(mpp_packet_init(&s->pending,nullptr,0)!=MPP_OK)return false;
            mpp_packet_set_eos(s->pending);s->eosSent=true;continue;
        }
        if(got!=AVERROR(EAGAIN))return false;
        if(s->eof)return false;
        got=av_read_frame(s->input,s->packet);
        if(got==AVERROR_EOF){s->eof=true;if(av_bsf_send_packet(s->filter,nullptr)<0)return false;continue;}
        if(got<0)return false;
        if(s->packet->stream_index!=s->stream){av_packet_unref(s->packet);continue;}
        if(av_bsf_send_packet(s->filter,s->packet)<0)return false;
    }
    if(!stopped_->load())Logger::error("MPP","decode/read timed out after 8 seconds");return false;
}
}
