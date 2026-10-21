#include "msad/TensorView.h"
#include <iostream>
#include <cmath>
#include <stdexcept>
using msad::TensorView;
int main() {
    try {
        const int8_t q[]={-128,12,0,12,127,-32};
        float f[6]; for(int i=0;i<6;++i) f[i]=(q[i]+12)*.01f;
        TensorView a(q,sizeof(q),6,TensorView::Type::Int8,.01f,-12);
        TensorView b(f,sizeof(f),6,TensorView::Type::Float32);
        for(size_t p=0;p<2;++p){int x,y;float s,t;a.bestClass(2,p,3,x,s);b.bestClass(2,p,3,y,t);
            if(x!=y||std::abs(s-t)>1e-6f)throw std::runtime_error("quantized class selection mismatch");}
        for(size_t i=0;i<6;++i)if(std::abs(a.value(i)-b.value(i))>1e-6f)throw std::runtime_error("dequant mismatch");
        bool rejected=false;try{TensorView bad(q,1,6,TensorView::Type::Int8);}catch(const std::invalid_argument&){rejected=true;}
        if(!rejected)throw std::runtime_error("short output accepted");
        const uint8_t u[]={0,255};TensorView c(u,2,2,TensorView::Type::Uint8,.5f,128);
        if(c.value(0)!=-64||c.value(1)!=63.5f)throw std::runtime_error("unsigned dequant mismatch");
        std::cout<<"tensor tests passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
