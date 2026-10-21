#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
namespace msad {
class DmaBuffer {
public:
    static std::shared_ptr<DmaBuffer> allocate(size_t bytes);
    ~DmaBuffer();
    DmaBuffer(const DmaBuffer&)=delete;
    DmaBuffer& operator=(const DmaBuffer&)=delete;
    int fd() const {return fd_;}
    uint8_t* data() const {return data_;}
    size_t size() const {return size_;}
    bool copyRgb(int width,int height,int stride,std::vector<uint8_t>& out) const;
private:
    DmaBuffer()=default;
    int fd_=-1;uint8_t* data_=nullptr;size_t size_=0;
};
}
