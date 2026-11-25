#include "photo_frame/frame_composite.h"
#include "photo_frame/frame_exif.h"
#include "metadata/exif_repack.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>

using namespace hdrbridge;
static void require(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
static void near(double a, double b, double eps, const char* what) { require(std::abs(a-b)<=eps, what); }

// Direct zero-extended 2D convolution, deliberately independent of the optimized CDF passes.
static std::vector<float> reference(const std::vector<float>& m,int w,int h,double sigma) {
  int r=int(std::ceil(3*sigma)); std::vector<double> k(2*r+1); double sum=0;
  for(int i=-r;i<=r;++i) sum += k[i+r]=std::exp(-i*i/(2*sigma*sigma));
  for(auto& x:k) x/=sum;
  std::vector<float> result(w*h);
  for(int y=0;y<h;++y) for(int x=0;x<w;++x) {
    double v=0;
    for(int j=-r;j<=r;++j) for(int i=-r;i<=r;++i)
      if(x+i>=0 && x+i<w && y+j>=0 && y+j<h) v+=m[(y+j)*w+x+i]*k[i+r]*k[j+r];
    result[y*w+x]=float(v*.4);
  }
  return result;
}
static void checkShadow(const std::vector<float>& m,int w,int h,float sigma) {
  auto a=frame::shadow(m,w,h,sigma), b=reference(m,w,h,sigma);
  for(size_t i=0;i<a.size();++i) near(a[i],b[i],1e-6,"shadow differs from direct convolution");
}
static void mathTests() {
  std::mt19937 gen(23); std::uniform_real_distribution<float> d(0,1);
  for(int h=1;h<=8;++h) for(int w=1;w<=9;++w) {
    std::vector<float> m(w*h); for(auto& x:m)x=d(gen);
    for(float s:{.35f,1.f,2.3f}) checkShadow(m,w,h,s);
  }
  std::vector<float> mask(25*21);
  for(int y=0;y<21;++y)for(int x=0;x<25;++x) mask[y*25+x]=frame::coverage(x+.5f,y+.5f,3,4,17,13);
  checkShadow(mask,25,21,1.7f);
  near(frame::coverage(10,10,3,4,17,13),1,1e-7,"coverage center");
  near(frame::coverage(-10,-10,3,4,17,13),0,1e-7,"coverage exterior");
  std::vector<frame::Pixel> line={{1,2,3},{4,5,6},{7,8,9}};
  frame::box_axis(line,3,1,2,false);
  near(line[0].r,2.8,1e-6,"box left edge"); near(line[2].r,5.2,1e-6,"box right edge");
  std::vector<frame::Pixel> constant(12,{.3f,.5f,.7f});
  for(int n=0;n<3;++n) { frame::box_axis(constant,4,3,8,false); frame::box_axis(constant,4,3,8,true); }
  for(auto p:constant) {near(p.r,.3,1e-6,"box constant r"); near(p.g,.5,1e-6,"box constant g");near(p.b,.7,1e-6,"box constant b");}
}

// Small synthetic TIFF writer with explicit directories and separately allocated payloads.
struct Tiff {
 bool le; std::vector<uint8_t> b;
 explicit Tiff(bool little):le(little),b(8,0) { b[0]=b[1]=le?'I':'M'; put16(2,42); put32(4,8); }
 void put16(size_t p,uint16_t v) { b[p]=uint8_t(v>>(le?0:8)); b[p+1]=uint8_t(v>>(le?8:0)); }
 void put32(size_t p,uint32_t v) { for(int i=0;i<4;++i)b[p+i]=uint8_t(v>>(8*(le?i:3-i))); }
 size_t dir(int n) { size_t p=b.size(); b.resize(p+2+12*n+4); put16(p,n); return p; }
 void entry(size_t dir,int i,uint16_t tag,uint16_t type,uint32_t count,uint32_t value) {
   size_t p=dir+2+12*i; put16(p,tag);put16(p+2,type);put32(p+4,count);put32(p+8,value);
 }
 void shortEntry(size_t dir,int i,uint16_t tag,uint16_t value) { entry(dir,i,tag,3,1,0);put16(dir+2+12*i+8,value); }
 uint32_t text(const char* s) { uint32_t p=uint32_t(b.size()); while(*s)b.push_back(uint8_t(*s++)); b.push_back(0);return p; }
 uint32_t rationals(std::initializer_list<uint32_t> values) { uint32_t p=uint32_t(b.size()); b.resize(p+4*values.size());size_t i=0;for(auto v:values)put32(p+4*i++,v);return p; }
};
static uint16_t read16(const std::vector<uint8_t>& b,size_t p,bool le) { return le?uint16_t(b.at(p)|(b.at(p+1)<<8)):uint16_t((b.at(p)<<8)|b.at(p+1)); }
static uint32_t read32(const std::vector<uint8_t>& b,size_t p,bool le) { uint32_t v=0;for(int i=0;i<4;++i)v|=uint32_t(b.at(p+i))<<(8*(le?i:3-i));return v; }
static size_t findEntry(const std::vector<uint8_t>& b,size_t dir,uint16_t tag,bool le) {
 auto n=read16(b,dir,le); for(unsigned i=0;i<n;++i){size_t p=dir+2+12*i;if(read16(b,p,le)==tag)return p;} return 0;
}
static void exifTest(bool le) {
 Tiff t(le); auto root=t.dir(7); auto ex=t.dir(4); auto gps=t.dir(4);
 auto make=t.text("Canon"),model=t.text("Canon EOS R5"),ap=t.rationals({28,10}),exp=t.rationals({1,125});
 auto lat=t.rationals({30,1,15,1,0,1}),lon=t.rationals({120,1,30,1,0,1});
 t.entry(root,0,0x010f,2,6,make);t.entry(root,1,0x0110,2,13,model);t.shortEntry(root,2,0x0112,6);
 t.entry(root,3,0x8769,4,1,uint32_t(ex));t.entry(root,4,0x8825,4,1,uint32_t(gps));t.entry(root,5,0x0111,4,1,123456);t.entry(root,6,0x0117,4,1,400);
 t.entry(ex,0,0x829d,5,1,ap);t.entry(ex,1,0x829a,5,1,exp);t.shortEntry(ex,2,0x8827,400);t.entry(ex,3,0x927c,7,8,0xfffffff0);
 t.entry(gps,0,1,2,2,0);t.b[gps+10]='N'; t.entry(gps,1,2,5,3,lat);t.entry(gps,2,3,2,2,0);t.b[gps+2+2*12+8]='E';t.entry(gps,3,4,5,3,lon);
 auto out=metadata::repack_exif(t.b,640,480);auto j=frame_exif::parse(out);
 require(!j.value("readError",false),"valid output EXIF");
 require(j.value("Make","")=="Canon" && j.value("Model","")=="Canon EOS R5","camera preserved");
 require(j.value("FNumber","")=="2.8" && j.value("ExposureTime","")=="1/125" && j.value("ISO","")=="400","exposure preserved");
 near(std::stod(j.value("GPSLatitude","0")),30.25,1e-6,"GPS latitude");near(std::stod(j.value("GPSLongitude","0")),120.5,1e-6,"GPS longitude");
 require(j.value("Orientation","")=="1","orientation normalized");
 auto dir=read32(out,4,le); auto width=findEntry(out,dir,0x100,le),height=findEntry(out,dir,0x101,le);
 require(width && height && read32(out,width+8,le)==640 && read32(out,height+8,le)==480,"root dimensions");
 require(!findEntry(out,dir,0x111,le) && !findEntry(out,dir,0x117,le),"strip removed");
 auto ptr=findEntry(out,dir,0x8769,le);require(ptr,"EXIF directory present");auto sub=read32(out,ptr+8,le);
 require(!findEntry(out,sub,0x927c,le),"invalid maker note removed");
 auto x=findEntry(out,sub,0xa002,le),y=findEntry(out,sub,0xa003,le);
 require(x && y && read32(out,x+8,le)==640 && read32(out,y+8,le)==480,"EXIF dimensions");
}

static void tiffMetadataTest() {
 Tiff t(true);
 const auto root = t.dir(4);
 const auto exif = t.dir(1);
 t.entry(root, 0, 0x010f, 2, 6, t.text("Canon"));
 t.entry(root, 1, 0x8769, 4, 1, uint32_t(exif));
 t.entry(root, 2, 0x0111, 4, 2, 0xfffffff0);
 t.entry(root, 3, 0x83bb, 4, 40, 0xfffffff0);
 t.entry(exif, 0, 0x829a, 5, 1, t.rationals({1, 160}));
 const auto result = metadata::repack([&](size_t offset, size_t count) {
   require(offset <= t.b.size() && count <= t.b.size() - offset, "TIFF metadata accessed a raster/private payload");
   return std::vector<uint8_t>(t.b.begin() + offset, t.b.begin() + offset + count);
 }, 640, 480, true);
 const auto fields = frame_exif::parse(result);
 require(fields.value("Make", "") == "Canon" && fields.value("ExposureTime", "") == "1/160",
         "TIFF camera fields preserved");
}

int main() { try { mathTests();exifTest(true);exifTest(false);tiffMetadataTest();std::cout<<"frame math and EXIF tests passed\n";return 0; } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;} }
