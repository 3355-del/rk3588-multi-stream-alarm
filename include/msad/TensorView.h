#ifndef MSAD_TENSOR_VIEW_H
#define MSAD_TENSOR_VIEW_H
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <stdexcept>
namespace msad {
// Contiguous NCHW output only. SDK-native blocked layouts must not use this view.
class TensorView {
public:
    enum class Type { Float32, Int8, Uint8 };
    TensorView(const void* data, size_t bytes, size_t count, Type type, float scale=1, int zp=0)
        : data_(data), count_(count), type_(type), scale_(scale), zp_(zp) {
        if (!data || count==0 || count>bytes/(type==Type::Float32?sizeof(float):1))
            throw std::invalid_argument("short output tensor");
        if (type!=Type::Float32 && (!std::isfinite(scale)||scale<=0))
            throw std::invalid_argument("invalid affine quantization scale");
    }
    float value(size_t i) const {
        if (i>=count_) throw std::out_of_range("tensor index");
        if(type_==Type::Float32)return static_cast<const float*>(data_)[i];
        return static_cast<float>(raw(i)-zp_)*scale_;
    }
    void bestClass(size_t cells,size_t pos,int classes,int& id,float& score) const {
        if(classes<1||pos>=cells||cells>count_/static_cast<size_t>(classes))
            throw std::invalid_argument("class tensor shape");
        id=-1;score=0;
        if(type_==Type::Float32){
            for(int c=0;c<classes;++c){float s=value(static_cast<size_t>(c)*cells+pos);if(s>score){score=s;id=c;}}
        }else{
            int best=-1000;
            for(int c=0;c<classes;++c){int q=raw(static_cast<size_t>(c)*cells+pos);if(q>best){best=q;id=c;}}
            score=static_cast<float>(best-zp_)*scale_;
        }
    }
private:
    int raw(size_t i) const {return type_==Type::Int8?static_cast<const int8_t*>(data_)[i]:static_cast<const uint8_t*>(data_)[i];}
    const void* data_;size_t count_;Type type_;float scale_;int zp_;
};
}
#endif
