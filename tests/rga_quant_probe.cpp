// Diagnostic only: determine whether installed RGA NN/ROP operations produce signed bytes.
#include "msad/DmaBuffer.h"
#include <im2d.h>
#include <iostream>
int main(){
    auto a=msad::DmaBuffer::allocate(16*16*3),b=msad::DmaBuffer::allocate(16*16*3);
    if(!a||!b)return 1;
    auto src=wrapbuffer_fd(a->fd(),16,16,RK_FORMAT_RGB_888);
    auto dst=wrapbuffer_fd(b->fd(),16,16,RK_FORMAT_RGB_888);
    for(int value:{0,127,128,255}){
        im_rect rect{0,0,16,16};
        if(imfill(src,rect,static_cast<int>(0xff000000U|static_cast<unsigned>(value)*0x010101U))!=IM_STATUS_SUCCESS)return 2;
        im_nn_t nn{};nn.scale_r=nn.scale_g=nn.scale_b=256;nn.offset_r=nn.offset_g=nn.offset_b=128;
        auto status=imquantize(src,dst,nn);std::vector<uint8_t> rgb;
        if(status==IM_STATUS_SUCCESS&&b->copyRgb(16,16,16,rgb))std::cout<<"NN "<<value<<" -> "<<int(rgb[0])<<'\n';
        else std::cout<<"NN unsupported: "<<imStrError(status)<<'\n';
        if(imfill(dst,rect,static_cast<int>(0xff808080U))!=IM_STATUS_SUCCESS)return 3;
        status=imrop(src,dst,IM_ROP_XOR);
        if(status==IM_STATUS_SUCCESS&&b->copyRgb(16,16,16,rgb))std::cout<<"ROP "<<value<<" -> "<<int(rgb[0])<<'\n';
        else std::cout<<"ROP unsupported: "<<imStrError(status)<<'\n';
    }
}
