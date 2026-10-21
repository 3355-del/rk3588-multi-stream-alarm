#include "msad/DmaBuffer.h"
#include <linux/dma-heap.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
namespace msad {
namespace {
int syncFd(int fd,uint64_t flags){dma_buf_sync sync{};sync.flags=flags;int result;do{result=ioctl(fd,DMA_BUF_IOCTL_SYNC,&sync);}while(result<0&&errno==EINTR);return result;}
}
std::shared_ptr<DmaBuffer> DmaBuffer::allocate(size_t bytes){
    if(!bytes||bytes>64U*1024*1024)return {};
    // Contiguous reserved heap is compatible with both RGA and NPU on this board.
    int heap=open("/dev/dma_heap/reserved",O_RDONLY|O_CLOEXEC);
    if(heap<0)heap=open("/dev/dma_heap/system-uncached",O_RDONLY|O_CLOEXEC);
    if(heap<0)return {};
    dma_heap_allocation_data request{};request.len=(bytes+4095)&~size_t(4095);request.fd_flags=O_RDWR|O_CLOEXEC;
    const int result=ioctl(heap,DMA_HEAP_IOCTL_ALLOC,&request);close(heap);
    if(result<0)return {};
    auto buffer=std::shared_ptr<DmaBuffer>(new DmaBuffer);
    buffer->fd_=static_cast<int>(request.fd);buffer->size_=static_cast<size_t>(request.len);
    void* mapped=mmap(nullptr,buffer->size_,PROT_READ|PROT_WRITE,MAP_SHARED,buffer->fd_,0);
    if(mapped==MAP_FAILED)return {};
    buffer->data_=static_cast<uint8_t*>(mapped);return buffer;
}
DmaBuffer::~DmaBuffer(){if(data_)munmap(data_,size_);if(fd_>=0)close(fd_);}
bool DmaBuffer::copyRgb(int width,int height,int stride,std::vector<uint8_t>& out) const{
    if(width<1||height<1||stride<width||static_cast<size_t>(stride)*static_cast<size_t>(height)*3>size_)return false;
    out.resize(static_cast<size_t>(width)*static_cast<size_t>(height)*3);
    if(syncFd(fd_,DMA_BUF_SYNC_START|DMA_BUF_SYNC_READ)<0)return false;
    for(int row=0;row<height;++row)std::memcpy(out.data()+static_cast<size_t>(row)*static_cast<size_t>(width)*3,
        data_+static_cast<size_t>(row)*static_cast<size_t>(stride)*3,static_cast<size_t>(width)*3);
    return syncFd(fd_,DMA_BUF_SYNC_END|DMA_BUF_SYNC_READ)==0;
}
}
