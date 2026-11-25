#include "photo_frame/photo_frame.h"
#include "photo_frame/frame_composite.h"
#include "photo_frame/frame_exif.h"
#include "canonical/cache.h"
#include "canonical/input.h"
#include "codecs/png.h"
#include "codecs/ultrahdr.h"
#include "codecs/uhdr_helpers.h"
#include "color/gamut.h"
#include "color/pq.h"
#include "infrastructure/source_revision.h"
#include "infrastructure/progress.h"
#include "infrastructure/timing.h"
#include "infrastructure/files.h"
#include "infrastructure/output_transaction.h"
#include "diagnostics/digest.h"
#include <lcms2.h>
#include <csetjmp>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <jpeglib.h>
#include <ultrahdr/ultrahdrcommon.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"

namespace hdrbridge::detail {

#if defined(_WIN32) || defined(__ANDROID__)
struct FrameJpegError { jpeg_error_mgr base; jmp_buf jump; };
static void frame_jpeg_error(j_common_ptr p) { longjmp(reinterpret_cast<FrameJpegError*>(p->err)->jump, 1); }
#endif
struct FramePair {
  DecodedImage hdr;
  std::vector<frame::Pixel> sdr; // linear Rec.2020, in units of 203 nits
  int origin_x=0, origin_y=0, source_w=0, source_h=0;
  unsigned interior_error=0, decoration_error=0;
};

struct FrameLogo {
  std::string svg;
  std::string signature;
};

static FrameLogo frame_load_logo(const std::filesystem::path& folder,const std::string& key) {
  if(key.empty() || folder.empty())return {};
  const auto path=folder/(key+"-w.svg");
  if(!std::filesystem::exists(path))return {};
  const auto bytes=read_file(path);
  if(bytes.empty() || bytes.size()>1024*1024)throw std::runtime_error("invalid camera logo size");
  return {std::string(bytes.begin(),bytes.end()),sha256(bytes)};
}

static FramePair frame_compose_hdr(const DecodedImage& source,
                            const ProgressCallback& progress,
                            std::atomic_bool* cancel,
                            const std::vector<uint8_t>& font_bytes,
                            const std::string& caption1, const std::string& caption2,
                            const FrameLogo& logo) {
  const int w = static_cast<int>(source.info.width);
  const int h = static_cast<int>(source.info.height);
  if (w <= 0 || h <= 0 || source.rgb.size() != size_t(w)*h*3)
    throw std::runtime_error("invalid HDR frame source");

  std::vector<uint32_t> rgba(size_t(w)*h);
  {
    DecodedImage p3; p3.info=source.info;
    p3.rgb = convert_rec2020_pq_to_p3_pq(source);
    auto packed = pq16_to_rgba1010102(p3);
    uhdr_raw_image_t hdr{}, sdr{};
    hdr.fmt=UHDR_IMG_FMT_32bppRGBA1010102; hdr.cg=UHDR_CG_DISPLAY_P3;
    hdr.ct=UHDR_CT_PQ; hdr.range=UHDR_CR_FULL_RANGE; hdr.w=w; hdr.h=h;
    hdr.planes[UHDR_PLANE_PACKED]=packed.data(); hdr.stride[UHDR_PLANE_PACKED]=w;
    sdr.fmt=UHDR_IMG_FMT_32bppRGBA8888; sdr.cg=UHDR_CG_DISPLAY_P3;
    sdr.ct=UHDR_CT_SRGB; sdr.range=UHDR_CR_FULL_RANGE; sdr.w=w; sdr.h=h;
    sdr.planes[UHDR_PLANE_PACKED]=rgba.data(); sdr.stride[UHDR_PLANE_PACKED]=w;
    ultrahdr::UltraHdr tone;
    require_uhdr(tone.toneMap(&hdr,&sdr),"tone-map frame base");
  }
  auto srgb=[](float v) { return v<=.04045f ? v/12.92f : std::pow((v+.055f)/1.055f,2.4f); };
  std::vector<frame::Pixel> base(size_t(w)*h), flattened(base.size());
  const auto* b=reinterpret_cast<const uint8_t*>(rgba.data());
  for(size_t i=0;i<base.size();++i) {
    float r=srgb(b[4*i]/255.f),g=srgb(b[4*i+1]/255.f),bl=srgb(b[4*i+2]/255.f);
    base[i]=frame::Pixel{.753833f*r+.198597f*g+.047570f*bl,
             .045744f*r+.941777f*g+.0124789f*bl,
             -.001210f*r+.017602f*g+.983608f*bl};
    float a=source.alpha.empty()?1.f:source.alpha[i]/65535.f;
    flattened[i]={base[i].r*a,base[i].g*a,base[i].b*a};
  }
  check_cancel(cancel);
  report(progress,48,"Rendering frame background");
  if (font_bytes.empty()) throw std::runtime_error("frame font is missing");
  stbtt_fontinfo font{};
  if (!stbtt_InitFont(&font,font_bytes.data(),stbtt_GetFontOffsetForIndex(font_bytes.data(),0)))
    throw std::runtime_error("invalid frame font");
  auto utf8=[](const std::string& text) {
    std::vector<int> out;for(size_t i=0;i<text.size();) {unsigned char a=text[i++];int c=a, extra=0;
      if((a&0xe0)==0xc0){c=a&31;extra=1;}else if((a&0xf0)==0xe0){c=a&15;extra=2;}else if((a&0xf8)==0xf0){c=a&7;extra=3;}else if(a>=128) throw std::runtime_error("invalid UTF-8 frame caption");
      bool valid=true;for(int j=0;j<extra;++j){if(i>=text.size()||(static_cast<unsigned char>(text[i])&0xc0)!=0x80){valid=false;break;}c=(c<<6)|(static_cast<unsigned char>(text[i++])&63);}if(!valid || (extra==1&&c<0x80) || (extra==2&&c<0x800) || (extra==3&&c<0x10000) || c>0x10ffff || (c>=0xd800&&c<=0xdfff)) throw std::runtime_error("invalid UTF-8 frame caption");out.push_back(c);
    }return out;
  };
  struct Row { std::vector<int> text; float scale; int width; int height; int ascent; };
  int logo_width=0,logo_height=0,logo_gap=0;
  std::vector<uint8_t> logo_alpha;
  if(!logo.svg.empty()) {
    auto svg=logo.svg;svg.push_back(0);
    std::unique_ptr<NSVGimage,decltype(&nsvgDelete)> image(nsvgParse(svg.data(),"px",96),nsvgDelete);
    if(!image || !image->shapes || !std::isfinite(image->width) || !std::isfinite(image->height) || image->width<=0 || image->height<=0)
      throw std::runtime_error("invalid camera logo SVG");
    const float scale=stbtt_ScaleForPixelHeight(&font,.03f*h);
    int ascent=0;for(const char c:std::string("QSOPNYuiyl90")){int x0,y0,x1,y1;stbtt_GetCodepointBitmapBox(&font,c,scale,scale,&x0,&y0,&x1,&y1);ascent=std::max(ascent,-y0);}
    logo_height=std::max(1,ascent);const double lw=std::ceil(logo_height*double(image->width)/image->height);
    if(lw>65500)throw std::runtime_error("camera logo aspect ratio is too large");
    logo_width=std::max(1,int(lw));logo_gap=caption1.empty()?0:std::max(1,int(std::lround(.009f*h)));
    std::unique_ptr<NSVGrasterizer,decltype(&nsvgDeleteRasterizer)> rasterizer(nsvgCreateRasterizer(),nsvgDeleteRasterizer);
    if(!rasterizer)throw std::runtime_error("cannot create camera logo renderer");
    std::vector<uint8_t> rgba(size_t(logo_width)*logo_height*4);
    nsvgRasterize(rasterizer.get(),image.get(),0,0,float(logo_height)/image->height,rgba.data(),logo_width,logo_height,logo_width*4);
    logo_alpha.resize(size_t(logo_width)*logo_height);for(size_t i=0;i<logo_alpha.size();++i)logo_alpha[i]=rgba[i*4+3];
  }
  std::vector<Row> rows;
  auto make_row=[&](const std::string& text,float size) {
    if(text.empty()) return;
    float scale=stbtt_ScaleForPixelHeight(&font,size);
    int asc=0,desc=0,gap=0; stbtt_GetFontVMetrics(&font,&asc,&desc,&gap);
    auto codepoints=utf8(text);for(int c:codepoints) if(stbtt_FindGlyphIndex(&font,c)==0) throw std::runtime_error("frame font does not support a caption character (Unicode code point "+std::to_string(c)+")");float width=0; for(size_t i=0;i<codepoints.size();++i) {int advance=0,bearing=0;stbtt_GetCodepointHMetrics(&font,codepoints[i],&advance,&bearing);width+=advance*scale;if(i+1<codepoints.size())width+=stbtt_GetCodepointKernAdvance(&font,codepoints[i],codepoints[i+1])*scale;}
    rows.push_back({codepoints,scale,int(std::ceil(width))+60,int(std::ceil(std::max(size,(asc-desc)*scale))),int(std::ceil(asc*scale))});
  };
  if(caption1.empty() && logo_width) {
    const float size=.03f*h,scale=stbtt_ScaleForPixelHeight(&font,size);int asc,desc,gap;stbtt_GetFontVMetrics(&font,&asc,&desc,&gap);
    rows.push_back({{},scale,60,int(std::ceil(std::max(size,(asc-desc)*scale))),int(std::ceil(asc*scale))});
  } else make_row(caption1,.03f*h);
  if(logo_width && !rows.empty())rows.front().width+=logo_width+logo_gap;
  make_row(caption2,.022f*h);
  float cw=float(w); int row_height=0; for(auto& row:rows) {cw=std::max(cw,float(row.width));row_height+=row.height;}
  float ch=rows.empty()?float(h):std::max(float(h),float(int(std::floor(1.04f*h))+row_height));
  float pad=.04f*h, ratio=float(w)/h, a=cw+2*pad, z=ch+2*pad;
  float raww=std::max(a,z*ratio), rawh=std::max(z,a/ratio);
  int ow=int(std::floor(raww)), oh=int(std::floor(rawh));
  int contentx=int(std::floor(pad+(raww-a)/2)), oy=int(std::floor(pad+(rawh-z)/2));
  int ox=contentx+int(std::floor((cw-w)/2));
  auto background=frame::background(flattened,w,h,ow,oh,cancel);
  check_cancel(cancel);
  std::vector<float> mask(size_t(ow)*oh);
  for(int y=0;y<oh;++y) for(int x=0;x<ow;++x)
    mask[size_t(y)*ow+x]=frame::coverage(x+.5f,y+.5f,float(ox),float(oy),float(w),float(h));
  auto shadow=frame::shadow(mask,ow,oh,.025f*h,cancel);
  FramePair result;
  result.origin_x=ox;result.origin_y=oy;result.source_w=w;result.source_h=h;
  result.hdr.info=source.info; result.hdr.exif=source.exif; result.hdr.xmp=source.xmp;
  result.hdr.info.width=ow; result.hdr.info.height=oh;
  result.hdr.alpha.clear(); result.hdr.rgb.resize(size_t(ow)*oh*3);
  result.sdr.resize(size_t(ow)*oh);
  for(int y=0;y<oh;++y) {
    if (!(y%32)) check_cancel(cancel);
    for(int x=0;x<ow;++x) {
      size_t dst=size_t(y)*ow+x;
      auto bg=background[dst]; float factor=1-shadow[dst];
      bg={bg.r*factor,bg.g*factor,bg.b*factor};
      auto d=bg; float m=0; size_t src=0;
      if(x>=ox && x<ox+w && y>=oy && y<oy+h) {
        src=size_t(y-oy)*w+(x-ox); m=mask[dst]*(source.alpha.empty()?1.f:source.alpha[src]/65535.f);
        d=frame::mix(bg,base[src],m);
      }
      result.sdr[dst]=d;
      for(int c=0;c<3;++c) {
        float v=c==0?bg.r:c==1?bg.g:bg.b;
        if(m>0) {
          double hv=transfer::pq_to_nits(source.rgb[src*3+c]/65535.0)/203.0;
          v=float(v*(1-m)+hv*m);
        }
        result.hdr.rgb[dst*3+c]=m==1.f?source.rgb[src*3+c]:uint16_t(std::lround(65535*transfer::nits_to_pq(std::max(0.f,v)*203)));
      }
    }
  }
  int row_y=oy+int(std::floor(1.04f*h));
  size_t row_index=0;
  for(const auto& row:rows) {
    float cursor=contentx+(cw-row.width)*.5f+30;
    if(row_index==0 && logo_width) {
      const int lx=int(std::lround(cursor));
      const int ly=int(std::lround(row_y+row.ascent-logo_height+.03f*logo_height));
      for(int y=0;y<logo_height;++y)for(int x=0;x<logo_width;++x){
        const int px=lx+x,py=ly+y;if(px<0||py<0||px>=ow||py>=oh)continue;
        const float alpha=logo_alpha[size_t(y)*logo_width+x]/255.f;if(alpha==0)continue;
        const size_t i=size_t(py)*ow+px;auto& q=result.sdr[i];q=frame::mix(q,{1,1,1},alpha);
        for(int c=0;c<3;++c)result.hdr.rgb[i*3+c]=uint16_t(std::lround(65535*transfer::nits_to_pq(std::max(0.f,c==0?q.r:c==1?q.g:q.b)*203)));
      }
      cursor+=logo_width+logo_gap;
    }
    for(size_t ci=0;ci<row.text.size();++ci) {
      int c=row.text[ci];
      int gw=0,gh=0,xoff=0,yoff=0;
      unsigned char* glyph=stbtt_GetCodepointBitmap(&font,0,row.scale,c,&gw,&gh,&xoff,&yoff);
      int gx=int(std::floor(cursor))+xoff, gy=row_y+row.ascent+yoff;
      for(int y=0;y<gh;++y) for(int x=0;x<gw;++x) {
        int px=gx+x,py=gy+y;if(px<0||py<0||px>=ow||py>=oh) continue;
        float alpha=glyph[y*gw+x]/255.f;size_t i=size_t(py)*ow+px;
        auto& q=result.sdr[i];q=frame::mix(q,{1,1,1},alpha);
        for(int chn=0;chn<3;++chn) result.hdr.rgb[i*3+chn]=uint16_t(std::lround(65535*transfer::nits_to_pq(std::max(0.f,chn==0?q.r:chn==1?q.g:q.b)*203)));
      }
      stbtt_FreeBitmap(glyph,nullptr);int advance=0,bearing=0;stbtt_GetCodepointHMetrics(&font,c,&advance,&bearing);cursor+=advance*row.scale;if(ci+1<row.text.size())cursor+=stbtt_GetCodepointKernAdvance(&font,c,row.text[ci+1])*row.scale;
    }
    row_y+=row.height;
    ++row_index;
  }
  for(int y=0;y<oh;++y) for(int x=0;x<ow;++x) {
    size_t i=size_t(y)*ow+x;float m=mask[i];
    if(x>=ox&&x<ox+w&&y>=oy&&y<oy+h) {
      size_t src=size_t(y-oy)*w+(x-ox);
      if(m==1.f && (source.alpha.empty()||source.alpha[src]==65535))
        for(int c=0;c<3;++c) result.interior_error=std::max(result.interior_error,unsigned(std::abs(int(result.hdr.rgb[i*3+c])-int(source.rgb[src*3+c]))));
    }
    if(m==0.f) {
      auto q=result.sdr[i];float v[3]{q.r,q.g,q.b};
      for(int c=0;c<3;++c) {int expected=int(std::lround(65535*transfer::nits_to_pq(std::max(0.f,v[c])*203)));
        result.decoration_error=std::max(result.decoration_error,unsigned(std::abs(int(result.hdr.rgb[i*3+c])-expected)));}
    }
  }
  return result;
}

static std::vector<uint8_t> frame_output_icc(const std::string& gamut) {
  cmsCIExyY wp{.3127,.329,1};
  cmsCIExyYTRIPLE prim = gamut == "p3"
      ? cmsCIExyYTRIPLE{{.68,.32,1},{.265,.69,1},{.15,.06,1}}
      : cmsCIExyYTRIPLE{{.708,.292,1},{.17,.797,1},{.131,.046,1}};
  cmsFloat64Number params[7]{2.4,1/1.055,.055/1.055,1/12.92,.04045,0,0};
  cmsToneCurve* trc=cmsBuildParametricToneCurve(nullptr,4,params);
  cmsToneCurve* curves[3]{trc,trc,trc}; auto profile=cmsCreateRGBProfile(&wp,&prim,curves);
  if(trc) cmsFreeToneCurve(trc); if(!profile) throw std::runtime_error("cannot create frame RGB profile");
  cmsUInt32Number n=0;cmsSaveProfileToMem(profile,nullptr,&n);std::vector<uint8_t> bytes(n);
  if(!cmsSaveProfileToMem(profile,bytes.data(),&n)) {cmsCloseProfile(profile);throw std::runtime_error("cannot serialize frame RGB profile");}
  cmsCloseProfile(profile);bytes.resize(n);return bytes;
}
static std::vector<uint16_t> frame_output_pixels(const FramePair& pair,const std::string& gamut) {
  std::vector<uint16_t> pixels(pair.sdr.size()*3);
  auto enc=[](double v){v=std::clamp(v,0.,1.);v=v<=.0031308?12.92*v:1.055*std::pow(v,1/2.4)-.055;return uint16_t(std::lround(v*65535));};
  const bool p3 = gamut == "p3";
  for(size_t i=0;i<pair.sdr.size();++i) {auto p=pair.sdr[i];
    if(p3) {
      pixels[i*3]=enc(1.3435783*p.r-.2821797*p.g-.0613986*p.b);
      pixels[i*3+1]=enc(-.0652975*p.r+1.0757879*p.g-.0104904*p.b);
      pixels[i*3+2]=enc(.0028218*p.r-.0195985*p.g+1.0167767*p.b);
    } else {
      pixels[i*3]=enc(p.r);
      pixels[i*3+1]=enc(p.g);
      pixels[i*3+2]=enc(p.b);
    }
  } return pixels;
}
static std::vector<uint8_t> frame_encode_jpeg(const DecodedImage& d,const std::vector<uint16_t>& pixels,const ConversionOptions& options) {
#if defined(_WIN32) || defined(__ANDROID__)
  jpeg_compress_struct j{};FrameJpegError err{};j.err=jpeg_std_error(&err.base);err.base.error_exit=frame_jpeg_error;
  if(setjmp(err.jump)){jpeg_destroy_compress(&j);throw std::runtime_error("frame JPEG encoding failed");}
  jpeg_create_compress(&j);unsigned char* memory=nullptr;unsigned long size=0;jpeg_mem_dest(&j,&memory,&size);
  j.image_width=d.info.width;j.image_height=d.info.height;j.input_components=3;j.in_color_space=JCS_RGB;jpeg_set_defaults(&j);
  for(int i=0;i<3;++i){j.comp_info[i].h_samp_factor=1;j.comp_info[i].v_samp_factor=1;}
  jpeg_set_quality(&j,std::clamp(options.base_quality,1,100),TRUE);j.optimize_coding=FALSE;jpeg_start_compress(&j,TRUE);
  auto icc=frame_output_icc(options.output_gamut);std::vector<uint8_t> marker={'I','C','C','_','P','R','O','F','I','L','E',0,1,1};marker.insert(marker.end(),icc.begin(),icc.end());
  if(marker.size()>65533)throw std::runtime_error("frame ICC too large");jpeg_write_marker(&j,JPEG_APP0+2,marker.data(),static_cast<unsigned>(marker.size()));
  std::vector<uint8_t> row(size_t(d.info.width)*3);
  while(j.next_scanline<j.image_height){size_t off=size_t(j.next_scanline)*row.size();for(size_t i=0;i<row.size();++i)row[i]=uint8_t((pixels[off+i]+128)/257);JSAMPROW p=row.data();jpeg_write_scanlines(&j,&p,1);}
  jpeg_finish_compress(&j);std::vector<uint8_t> bytes(memory,memory+size);std::free(memory);jpeg_destroy_compress(&j);return bytes;
#else
  throw std::runtime_error("frame JPEG encoder unavailable");
#endif
}
static std::vector<uint8_t> frame_encode_uhdr(const FramePair& pair,const std::vector<uint16_t>& sdr_pixels,const std::vector<uint8_t>& jpeg,const HdrStats& stats,const ConversionOptions& options) {
  DecodedImage hdr_target;hdr_target.info=pair.hdr.info;hdr_target.rgb=options.output_gamut=="p3" ? convert_rec2020_pq_to_p3_pq(pair.hdr) : pair.hdr.rgb;auto packed=pq16_to_rgba1010102(hdr_target);
  std::vector<uint32_t> sdr(sdr_pixels.size()/3);auto* out=reinterpret_cast<uint8_t*>(sdr.data());for(size_t i=0;i<sdr.size();++i){for(int c=0;c<3;++c)out[4*i+c]=uint8_t((sdr_pixels[3*i+c]+128)/257);out[4*i+3]=255;}
  uhdr_raw_image_t h{},s{};h.fmt=UHDR_IMG_FMT_32bppRGBA1010102;h.cg=options.output_gamut=="p3" ? UHDR_CG_DISPLAY_P3 : UHDR_CG_BT_2100;h.ct=UHDR_CT_PQ;h.range=UHDR_CR_FULL_RANGE;h.w=pair.hdr.info.width;h.h=pair.hdr.info.height;h.planes[UHDR_PLANE_PACKED]=packed.data();h.stride[UHDR_PLANE_PACKED]=h.w;
  s=h;s.fmt=UHDR_IMG_FMT_32bppRGBA8888;s.ct=UHDR_CT_SRGB;s.planes[UHDR_PLANE_PACKED]=sdr.data();
  uhdr_compressed_image_t compressed{};compressed.data=const_cast<uint8_t*>(jpeg.data());compressed.data_sz=jpeg.size();compressed.capacity=jpeg.size();compressed.cg=h.cg;compressed.ct=UHDR_CT_SRGB;compressed.range=UHDR_CR_FULL_RANGE;
  UhdrEncoderPtr enc(uhdr_create_encoder());if(!enc)throw std::runtime_error("cannot create frame UHDR encoder");
  require_uhdr(uhdr_enc_set_raw_image(enc.get(),&h,UHDR_HDR_IMG),"set frame HDR");require_uhdr(uhdr_enc_set_raw_image(enc.get(),&s,UHDR_SDR_IMG),"set frame SDR");require_uhdr(uhdr_enc_set_compressed_image(enc.get(),&compressed,UHDR_SDR_IMG),"set frame JPEG");
  require_uhdr(uhdr_enc_set_gainmap_scale_factor(enc.get(),options.gainmap_scale),"set frame gain scale");require_uhdr(uhdr_enc_set_using_multi_channel_gainmap(enc.get(),options.multi_channel_gainmap ? 1 : 0),"set frame RGB gain");require_uhdr(uhdr_enc_set_quality(enc.get(),std::clamp(options.gainmap_quality,0,100),UHDR_GAIN_MAP_IMG),"set frame gain quality");
  require_uhdr(uhdr_enc_set_target_display_peak_brightness(enc.get(),float(std::max(204.0,stats.chosen_target_nits))),"set frame peak");require_uhdr(uhdr_encode(enc.get()),"encode frame UHDR");
  auto* stream=uhdr_get_encoded_stream(enc.get());if(!stream||!stream->data||!stream->data_sz)throw std::runtime_error("empty frame UHDR output");auto* first=static_cast<uint8_t*>(stream->data);return {first,first+stream->data_sz};
}

ConversionResult convert_photo_frame(const std::filesystem::path& input,const std::filesystem::path& output,
                                     const ConversionOptions& options,const ProgressCallback& progress,std::atomic_bool* cancel) {
  auto start=Clock::now();
  if(options.frame_format!="jpeg"&&options.frame_format!="png")throw std::runtime_error("frame format must be jpeg or png");
  if(options.output_gamut!="rec2020"&&options.output_gamut!="p3")throw std::runtime_error("output gamut must be rec2020 or p3");
  if(options.output_transfer!="pq"&&options.output_transfer!="hlg")throw std::runtime_error("output transfer must be pq or hlg");
  if(options.frame_format=="jpeg"&&options.output_transfer!="pq")throw std::runtime_error("frame JPEG output only supports pq transfer");
  if(options.frame_format=="jpeg" && options.gainmap_scale!=1 && options.gainmap_scale!=2 && options.gainmap_scale!=4) throw std::runtime_error("frame gain-map scale must be 1, 2, or 4");
  if(options.frame_font_path.empty())throw std::runtime_error("photo frame requires a font path");
  if(std::filesystem::exists(output)&&!options.overwrite)throw std::runtime_error("output exists; overwrite not enabled");
  const auto initial_revision=source_revision(std::filesystem::absolute(input));
  const auto initial_size=std::filesystem::file_size(input);
  const auto initial_time=std::filesystem::last_write_time(input);
  auto info=inspect_any(input);bool hdr=info.asset_kind=="direct-hdr"||info.asset_kind=="gain-map-hdr";
  if(!hdr) throw std::runtime_error("No HDR data");
  TimingDiagnostics timings;DecodedImage source=*decode_input(input,progress,cancel,timings);
  if(std::filesystem::file_size(input)!=initial_size || std::filesystem::last_write_time(input)!=initial_time || (initial_revision.valid && source_revision(std::filesystem::absolute(input))!=initial_revision)) throw std::runtime_error("source changed during frame decode");
  auto font=read_file(std::filesystem::u8path(options.frame_font_path));
  const auto fields=frame_exif::caption_fields(source.exif,source.xmp);
  auto captions=frame_exif::captions(fields);
  const auto key=frame_exif::camera_logo_key(fields.value("Make",std::string{}),fields.value("Model",std::string{}));
  auto logo_folder=options.frame_logo_path.empty() ? std::filesystem::u8path(options.frame_font_path).parent_path().parent_path()/"frame-logos" : std::filesystem::u8path(options.frame_logo_path);
  const auto logo=options.frame_caption1.empty()?frame_load_logo(logo_folder,key):FrameLogo{};
  if(logo.svg.empty() && !key.empty()) {
    const auto make=fields.value("Make",std::string{});captions.first=captions.first.empty()?make:make+" "+captions.first;
  }
  const auto c1=options.frame_caption1.empty()?captions.first:options.frame_caption1;
  const auto c2=options.frame_caption2.empty()?captions.second:options.frame_caption2;
  struct Cache { std::filesystem::path path,font; uintmax_t size=0,font_size=0; std::filesystem::file_time_type time{},font_time{}; SourceRevision revision; std::string c1,c2,logo_signature; std::shared_ptr<FramePair> pair; };
  static std::mutex cache_mutex; static Cache cache;
  auto path=std::filesystem::absolute(input),fontpath=std::filesystem::absolute(std::filesystem::u8path(options.frame_font_path));
  auto size=std::filesystem::file_size(path),font_size=std::filesystem::file_size(fontpath);
  auto time=std::filesystem::last_write_time(path),font_time=std::filesystem::last_write_time(fontpath);auto revision=source_revision(path);
  std::shared_ptr<FramePair> cached;
  {std::lock_guard lock(cache_mutex);if(revision.valid&&cache.pair&&cache.path==path&&cache.font==fontpath&&cache.size==size&&cache.font_size==font_size&&cache.time==time&&cache.font_time==font_time&&cache.revision==revision&&cache.c1==c1&&cache.c2==c2&&cache.logo_signature==logo.signature) cached=cache.pair;}
  const bool cache_hit=bool(cached);
  if(!cached) {
    cached=std::make_shared<FramePair>(frame_compose_hdr(source,progress,cancel,font,c1,c2,logo));
    if(std::filesystem::file_size(path)!=size||std::filesystem::last_write_time(path)!=time||(revision.valid&&source_revision(path)!=revision)||std::filesystem::file_size(fontpath)!=font_size||std::filesystem::last_write_time(fontpath)!=font_time) throw std::runtime_error("source or font changed during frame rendering");
    cached->hdr.exif.clear();cached->hdr.xmp.clear();
    if(revision.valid){std::lock_guard lock(cache_mutex);cache={path,fontpath,size,font_size,time,font_time,revision,c1,c2,logo.signature,cached};}
  }
  const FramePair& pair=*cached;
  check_cancel(cancel);report(progress,75,"Encoding framed image");
  std::vector<uint16_t> pixels;
  if(options.frame_format=="jpeg") pixels=frame_output_pixels(pair,options.output_gamut);
  std::vector<uint8_t> bytes;
  std::vector<uint16_t> converted_png;
  const std::vector<uint16_t>* png_pixels=&pair.hdr.rgb;
  bool expect_png_icc=false;
  if(options.frame_format=="png") {
    ConversionOptions pngopts=options;pngopts.copy_exif=false;pngopts.copy_xmp=false;
    converted_png=options.output_transfer=="hlg"
        ? convert_rec2020_pq_to_hlg(pair.hdr,options.output_gamut)
        : options.output_gamut=="p3" ? convert_rec2020_pq_to_p3_pq(pair.hdr)
                                     : std::vector<uint16_t>{};
    if(!converted_png.empty()) png_pixels=&converted_png;
    expect_png_icc=pngopts.output_transfer=="pq"&&pngopts.embed_hdr_icc;
    bytes=encode_png(pair.hdr,pngopts,*png_pixels);
  }else{
    auto jpeg=frame_encode_jpeg(pair.hdr,pixels,options);
    auto stats=measure_hdr(source);bytes=frame_encode_uhdr(pair,pixels,jpeg,stats,options);
  }
  if(!output.parent_path().empty())std::filesystem::create_directories(output.parent_path());OutputTransaction tx(output);std::string error;if(!tx.create(error))throw std::runtime_error(error);
  write_file(tx.stage_path(),bytes);check_cancel(cancel);
  Verification verification;
  if(options.frame_format=="png")verification=decode_png(bytes,png_pixels,expect_png_icc,false,nullptr);
  else verification=decode_ultrahdr(bytes);
  verification.checks.push_back("frame main image origin="+std::to_string(pair.origin_x)+","+std::to_string(pair.origin_y)+" size="+std::to_string(pair.source_w)+"x"+std::to_string(pair.source_h));
  verification.checks.push_back("frame source interior maximum PQ16 code error="+std::to_string(pair.interior_error));
  verification.checks.push_back("frame decoration maximum paired PQ16 code error="+std::to_string(pair.decoration_error));
  verification.checks.push_back("frame camera logo="+(logo.svg.empty()?std::string("none"):key)+"; model="+c1);
  verification.passed=verification.passed && pair.interior_error==0 && pair.decoration_error<=1;
  if(!verification.passed)throw std::runtime_error("frame output verification failed");
  ConversionResult result;result.success=true;result.output_path=output;result.output_bytes=bytes.size();result.mode=options.mode;result.sha256=sha256(bytes);result.verification=std::move(verification);result.timings=timings;result.timings.canonical_cache_hit=cache_hit;
  check_cancel(cancel);if(!tx.commit(options.overwrite,error))throw std::runtime_error(error);result.timings.total_ms=elapsed_ms(start);report(progress,100,"Frame output verified and committed");return result;
}


}  // namespace hdrbridge::detail
