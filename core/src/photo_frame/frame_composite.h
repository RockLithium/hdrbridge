#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <vector>

// Linear-light primitives for the photo-frame compositor. RGB channels share
// the caller's luminance units; alpha is straight and is flattened on black.
namespace hdrbridge::frame {
struct Pixel { float r, g, b; };
inline Pixel mix(Pixel a, Pixel b, float t) {
  return {a.r+(b.r-a.r)*t,a.g+(b.g-a.g)*t,a.b+(b.b-a.b)*t};
}
inline Pixel sample(const std::vector<Pixel>& p, int w, int h, float x, float y) {
  x=std::clamp(x,0.f,float(w-1)); y=std::clamp(y,0.f,float(h-1));
  int x0=int(x), y0=int(y), x1=std::min(x0+1,w-1), y1=std::min(y0+1,h-1);
  return mix(mix(p[y0*w+x0],p[y0*w+x1],x-x0),
             mix(p[y1*w+x0],p[y1*w+x1],x-x0),y-y0);
}
inline std::vector<Pixel> resize(const std::vector<Pixel>& p, int w, int h, int nw, int nh) {
  if (w<=0||h<=0||nw<=0||nh<=0||p.size()!=size_t(w)*h) throw std::runtime_error("invalid frame image size");
  std::vector<Pixel> out(size_t(nw)*nh);
  for(int y=0;y<nh;++y) for(int x=0;x<nw;++x)
    out[size_t(y)*nw+x]=sample(p,w,h,(x+.5f)*w/nw-.5f,(y+.5f)*h/nh-.5f);
  return out;
}
inline void box_axis(std::vector<Pixel>& p, int w, int h, int radius, bool vertical, std::atomic_bool* cancel = nullptr) {
  std::vector<Pixel> out(p.size());
  int lines=vertical?w:h, length=vertical?h:w;
  auto idx=[&](int line,int pos)->size_t { return vertical ? size_t(pos)*w+line : size_t(line)*w+pos; };
  for(int l=0;l<lines;++l) {
    if(cancel && !(l%32) && cancel->load()) throw std::runtime_error("conversion cancelled");
    double r=0,g=0,b=0;
    auto add=[&](int pos,int sign) { auto a=p[idx(l,std::clamp(pos,0,length-1))]; r+=sign*a.r;g+=sign*a.g;b+=sign*a.b; };
    for(int i=-radius;i<=radius;++i) add(i,1);
    float inv=1.f/(2*radius+1);
    for(int x=0;x<length;++x) { out[idx(l,x)]={float(r*inv),float(g*inv),float(b*inv)}; add(x-radius,-1);add(x+radius+1,1); }
  }
  p.swap(out);
}
inline std::vector<Pixel> background(const std::vector<Pixel>& source,int w,int h,int outw,int outh,std::atomic_bool* cancel = nullptr) {
  constexpr int side=3025;
  auto p=resize(source,w,h,side,side);
  for(int i=0;i<2;++i) { box_axis(p,side,side,200,false,cancel);box_axis(p,side,side,200,true,cancel); }
  auto out=resize(p,side,side,outw,outh);
  for(auto& v:out) {v.r*=.8f;v.g*=.8f;v.b*=.8f;}
  return out;
}
inline float coverage(float px,float py,float x,float y,float w,float h) {
  float r=.013f*h;
  float dx=std::abs(px-(x+w*.5f))-(w*.5f-r);
  float dy=std::abs(py-(y+h*.5f))-(h*.5f-r);
  float d=std::hypot(std::max(dx,0.f),std::max(dy,0.f))+std::min(std::max(dx,dy),0.f)-r;
  return std::clamp(.5f-d,0.f,1.f);
}
inline std::vector<float> shadow(const std::vector<float>& mask,int w,int h,float sigma,std::atomic_bool* cancel = nullptr) {
  if (w<=0||h<=0||mask.size()!=size_t(w)*h||sigma<=0) throw std::runtime_error("invalid frame shadow");
  int radius=int(std::ceil(3*sigma)); std::vector<double> cdf(size_t(radius)*2+1);
  double sum=0; for(int i=-radius;i<=radius;++i) {double v=std::exp(-double(i)*i/(2*double(sigma)*sigma));cdf[size_t(i+radius)]=v;sum+=v;}
  for(auto& v:cdf)v/=sum;
  for(size_t i=1;i<cdf.size();++i)cdf[i]+=cdf[i-1];
  // The discrete derivative of a rounded rectangle is sparse. Convolving it
  // with the cumulative finite Gaussian gives the same kernel convolution,
  // without visiting hundreds of identical mask samples at every pixel.
  auto axis=[&](const std::vector<float>& input,bool vertical) {
    std::vector<float> output(input.size());
    const int lines=vertical?w:h,length=vertical?h:w;
    std::vector<double> values(length),tails(size_t(length)+1);
    for(int line=0;line<lines;++line){
      if(cancel && !(line%32) && cancel->load())throw std::runtime_error("conversion cancelled");
      std::fill(values.begin(),values.end(),0.);std::fill(tails.begin(),tails.end(),0.);
      const auto index=[&](int x){return vertical?size_t(x)*w+line:size_t(line)*w+x;};
      double previous=0;
      for(int i=0;i<=length;++i){const double current=i==length?0.:input[index(i)],delta=current-previous;previous=current;if(delta==0.)continue;
        const int first=std::max(0,i-radius),last=std::min(length-1,i+radius);
        for(int x=first;x<=last;++x)values[x]+=delta*cdf[size_t(x-i+radius)];
        const int tail=std::max(0,i+radius+1);if(tail<length)tails[tail]+=delta;
      }
      double accumulated=0;for(int x=0;x<length;++x){accumulated+=tails[x];output[index(x)]=float(values[x]+accumulated);}
    }
    return output;
  };
  auto tmp=axis(mask,false);auto out=axis(tmp,true);
  for(auto& value:out)value*=.4f;
  return out;
}
} // namespace hdrbridge::frame
