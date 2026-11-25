#include "hdrbridge_core.h"
#include <png.h>
#include <zlib.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs=std::filesystem;
using namespace hdrbridge;
static void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
struct Temp {
  fs::path dir;
  Temp(){auto n=std::chrono::high_resolution_clock::now().time_since_epoch().count();dir=fs::temp_directory_path()/("hdrbridge-frame-integration-"+std::to_string(n));require(fs::create_directory(dir),"cannot create unique test directory");}
  ~Temp(){std::error_code e;fs::remove_all(dir,e);}
};
static void writer(png_structp p,png_bytep data,png_size_t n){auto* v=static_cast<std::vector<uint8_t>*>(png_get_io_ptr(p));v->insert(v->end(),data,data+n);}
static void flush(png_structp){}
static uint32_t be32(const uint8_t* p){return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];}
static std::vector<uint8_t> read(const fs::path& p){std::ifstream f(p,std::ios::binary);require(bool(f),"read test file");return {std::istreambuf_iterator<char>(f),{}};}
static std::vector<uint8_t> chunk(const std::vector<uint8_t>& bytes,const char* name){
  for(size_t p=8;p+12<=bytes.size();) {size_t n=be32(bytes.data()+p);if(n>bytes.size()-p-12)break;
    if(std::equal(name,name+4,bytes.begin()+p+4))return {bytes.begin()+p+8,bytes.begin()+p+8+n};p+=n+12;}
  return {};
}
static std::vector<uint8_t> fixtureExif(){
  // TIFF IFD0: Orientation=6, Make="TEST". Normalization rotates source.
  return {'I','I',42,0,8,0,0,0,2,0,
    0x12,0x01,3,0,1,0,0,0,6,0,0,0,
    0x0f,0x01,2,0,5,0,0,0,38,0,0,0,
    0,0,0,0,'T','E','S','T',0};
}
static void createPng(const fs::path& path,bool hdr,bool camera=true) {
  png_structp p=png_create_write_struct(PNG_LIBPNG_VER_STRING,nullptr,nullptr,nullptr);require(p,"create png writer");
  png_infop info=png_create_info_struct(p);require(info,"create png info");
  if(setjmp(png_jmpbuf(p)))throw std::runtime_error("create fixture png");
  std::vector<uint8_t> bytes;png_set_write_fn(p,&bytes,writer,flush);
  constexpr unsigned w=64,h=48;png_set_IHDR(p,info,w,h,16,PNG_COLOR_TYPE_RGB,PNG_INTERLACE_NONE,PNG_COMPRESSION_TYPE_BASE,PNG_FILTER_TYPE_BASE);
  auto exif=camera ? fixtureExif() : std::vector<uint8_t>{'I','I',42,0,8,0,0,0,1,0,0x12,0x01,3,0,1,0,0,0,6,0,0,0,0,0,0,0};png_set_eXIf_1(p,info,static_cast<png_uint_32>(exif.size()),exif.data());png_write_info(p,info);
  // libpng expects big-endian samples unless png_set_swap is used.
  std::vector<uint8_t> row(w*6);for(unsigned y=0;y<h;++y){for(unsigned x=0;x<w;++x)for(unsigned c=0;c<3;++c){uint16_t v=hdr?uint16_t(30000+x*130+y*75+c*500):uint16_t(4000+x*500+y*150+c*500);row[x*6+c*2]=uint8_t(v>>8);row[x*6+c*2+1]=uint8_t(v);}png_write_row(p,row.data());}
  png_write_end(p,info);png_destroy_write_struct(&p,&info);
  if(hdr){std::vector<uint8_t> c={0,0,0,4,'c','I','C','P',9,16,0,1,0,0,0,0};uLong crc=crc32(0L,Z_NULL,0);crc=crc32(crc,c.data()+4,8);for(int i=0;i<4;++i)c[12+i]=uint8_t(crc>>(24-8*i));bytes.insert(bytes.begin()+33,c.begin(),c.end());}
  std::ofstream f(path,std::ios::binary);f.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));require(bool(f),"write fixture");
}
static bool check(const Verification& v,const std::string& expected){return std::find(v.checks.begin(),v.checks.end(),expected)!=v.checks.end();}
static void pairChecks(const ConversionResult& r){require(r.success&&r.verification.passed,"frame conversion verified");require(check(r.verification,"frame source interior maximum PQ16 code error=0"),"source interior must be exact");require(check(r.verification,"frame decoration maximum paired PQ16 code error=0"),"decoration must match");}
int main(){try{
  Temp temp;auto src=temp.dir/"hdr.png",sdr=temp.dir/"sdr.png";createPng(src,true);createPng(sdr,false);
  ConversionOptions o;o.mode="photo-frame";o.frame_font_path=HDRBRIDGE_FRAME_FONT;o.frame_format="jpeg";
  o.output_gamut="p3";o.gainmap_scale=1;o.multi_channel_gainmap=true;
  // This fixture has only Make, so provide an explicit caption on the first run.
  o.frame_caption1="FRAME TEST";auto jpeg=convert(src,temp.dir/"frame.jpg",o);pairChecks(jpeg);
  require(jpeg.verification.gain_map_channels==3,"RGB gain map");require(jpeg.verification.gain_map_width==jpeg.verification.width&&jpeg.verification.gain_map_height==jpeg.verification.height,"full-resolution gain map");
  o.frame_format="png";o.output_transfer="hlg";
  auto png=convert(src,temp.dir/"frame.png",o);pairChecks(png);
  require(png.timings.canonical_cache_hit,"paired frame cache reused");
  require(png.verification.exact_roundtrip,"HDR PNG exact decoding");
  require(png.verification.width==jpeg.verification.width&&png.verification.height==jpeg.verification.height,"paired canvas dimensions");
  require(std::any_of(png.verification.checks.begin(),png.verification.checks.end(),[](const std::string& s){return s.find("frame main image origin=")==0 && s.find("size=48x64")!=std::string::npos;}),"source orientation and layout dimensions");
  auto encoded=read(temp.dir/"frame.png");require(chunk(encoded,"eXIf").empty(),"source EXIF must not be copied");
  require(chunk(encoded,"cICP")==std::vector<uint8_t>{12,18,0,1},"frame PNG signals Display P3/HLG");

  o.frame_format="jpeg";o.output_gamut="rec2020";o.output_transfer="pq";
  const auto jpeg2020Path=temp.dir/"frame-2020.jpg";
  const auto jpeg2020=convert(src,jpeg2020Path,o);
  require(jpeg2020.success&&jpeg2020.verification.passed,"Rec.2020 frame JPEG verified");
  require(inspect(jpeg2020Path).icc_primaries==9,"frame JPEG base ICC signals Rec.2020");

  o.frame_format="png";
  o.frame_caption1.clear();auto automatic=convert(src,temp.dir/"automatic.png",o);pairChecks(automatic);
  // A source without metadata gives empty automatic captions.
  o.frame_caption1.clear();auto noCaptionSource=temp.dir/"empty.png";createPng(noCaptionSource,true,false);
  auto noCaption=convert(noCaptionSource,temp.dir/"empty-frame.png",o);pairChecks(noCaption);
  require(automatic.verification.height>noCaption.verification.height,"source camera EXIF still supplies automatic caption");
  o.frame_caption1="SDR";bool frameRejected=false;try{convert(sdr,temp.dir/"sdr-frame.png",o);}catch(const std::exception& e){frameRejected=std::string(e.what()).find("No HDR data")!=std::string::npos;}require(frameRejected&&!fs::exists(temp.dir/"sdr-frame.png"),"SDR frame input rejected");
  ConversionOptions legacy;legacy.mode="png-pq16";bool rejected=false;try{convert(sdr,temp.dir/"legacy.png",legacy);}catch(const std::exception& e){rejected=std::string(e.what()).find("No HDR data")!=std::string::npos;}require(rejected,"legacy SDR conversion remains rejected");
  auto cancelled=temp.dir/"cancel.png";std::atomic_bool stop{true};bool cancelledError=false;try{convert(src,cancelled,o,{},&stop);}catch(const std::exception&){cancelledError=true;}require(cancelledError&&!fs::exists(cancelled),"cancel leaves no output");
  auto existing=temp.dir/"exists.png";{std::ofstream f(existing,std::ios::binary);f<<"preserve";}auto before=read(existing);bool overwriteError=false;try{convert(src,existing,o);}catch(const std::exception&){overwriteError=true;}require(overwriteError&&read(existing)==before,"existing output preserved");
  std::cout<<"photo frame integration tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
