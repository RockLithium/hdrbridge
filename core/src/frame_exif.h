#pragma once

// Small, bounds-checked TIFF/EXIF reader for frame captions.
// Reuses the Apache-2.0 HDR Bridge local prototype reader; no upstream Yiyin source.
#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <functional>
#include <map>
#include <utility>
#include <stdexcept>

namespace hdrbridge::frame_exif {
namespace detail {

class Reader {
public:
    explicit Reader(const std::vector<uint8_t>& data) : b(data), base(findBase(data)) {
        if (base == npos || base + 8 > b.size()) return;
        if (b[base] == 'I' && b[base + 1] == 'I') le = true;
        else if (b[base] == 'M' && b[base + 1] == 'M') le = false;
        else return;
        if (u16(base + 2) != 42) return;
        valid = true;
        firstIfd = u32(base + 4);
    }

    nlohmann::json run() {
        if (!valid) { result["readError"] = true; return result; }
        visit(firstIfd, 0);
        if (!error.empty()) result["readError"] = true;
        return result;
    }

private:
    static constexpr size_t npos = std::numeric_limits<size_t>::max();
    const std::vector<uint8_t>& b;
    size_t base = npos;
    bool le = true, valid = false;
    uint32_t firstIfd = 0;
    std::string error;
    std::vector<uint32_t> visited;
    nlohmann::json result = nlohmann::json::object();

    static size_t findBase(const std::vector<uint8_t>& x) {
        auto isTiff = [&](size_t p) { return p + 4 <= x.size() && ((x[p]=='I'&&x[p+1]=='I'&&x[p+2]==42&&x[p+3]==0)||(x[p]=='M'&&x[p+1]=='M'&&x[p+2]==0&&x[p+3]==42)); };
        if (isTiff(0)) return 0;
        if (x.size() >= 6 && x[0]=='E'&&x[1]=='x'&&x[2]=='i'&&x[3]=='f'&&x[4]==0&&x[5]==0 && isTiff(6)) return 6;
        if (x.size() >= 4) {
            uint32_t off = (uint32_t(x[0])<<24)|(uint32_t(x[1])<<16)|(uint32_t(x[2])<<8)|x[3];
            if (isTiff(off)) return off;
            if (off <= x.size() && x.size()-off >= 6 && x[off]=='E'&&x[off+1]=='x'&&x[off+2]=='i'&&x[off+3]=='f'&&x[off+4]==0&&x[off+5]==0 && isTiff(off+6)) return off+6;
            // Some containers store an offset relative to the bytes after the 4-byte field.
            if (off <= x.size()-std::min<size_t>(4,x.size()) && isTiff(size_t(off)+4)) return size_t(off)+4;
        }
        return npos;
    }
    bool has(size_t p, size_t n) const { return p <= b.size() && n <= b.size()-p; }
    uint16_t u16(size_t p) const { if(!has(p,2)) { const_cast<Reader*>(this)->error="truncated"; return 0; } return le ? uint16_t(b[p]|(uint16_t(b[p+1])<<8)) : uint16_t((uint16_t(b[p])<<8)|b[p+1]); }
    uint32_t u32(size_t p) const { if(!has(p,4)) { const_cast<Reader*>(this)->error="truncated"; return 0; } return le ? uint32_t(b[p])|(uint32_t(b[p+1])<<8)|(uint32_t(b[p+2])<<16)|(uint32_t(b[p+3])<<24) : (uint32_t(b[p])<<24)|(uint32_t(b[p+1])<<16)|(uint32_t(b[p+2])<<8)|b[p+3]; }
    std::string number(double d) const { std::ostringstream s; s << std::setprecision(8) << d; return s.str(); }
    size_t typeSize(uint16_t t) const { switch(t) { case 1: case 2: case 6: case 7:return 1; case 3: case 8:return 2; case 4: case 9: case 11: case 13:return 4; case 5: case 10: case 12:return 8; default:return 0; } }
    bool dataLoc(uint32_t off, uint32_t count, uint16_t type, size_t& p, size_t& total) {
        size_t z=typeSize(type); if(!z || count > std::numeric_limits<size_t>::max()/z) { error="invalid type/count"; return false; }
        total=z*size_t(count); p=total<=4 ? off : size_t(base)+off;
        if(p<base || !has(p,total)) { error="out of bounds"; return false; } return true;
    }
    double scalar(size_t p,uint16_t type) {
        switch(type) { case 1: case 6:return b[p]; case 3:return u16(p); case 4:return u32(p); case 8:return static_cast<int16_t>(u16(p)); case 9:return static_cast<int32_t>(u32(p)); case 5: case 10: {uint32_t n=u32(p),d=u32(p+4); if(!d){error="zero denominator";return 0;} return type==5?double(n)/d:double(static_cast<int32_t>(n))/static_cast<int32_t>(d);} default:return 0; }
    }
    std::string ascii(size_t p,size_t n) { size_t end=p; while(end<p+n && b[end]) ++end; return std::string(reinterpret_cast<const char*>(b.data()+p),end-p); }
    std::string exposure(double v) { if(v>0 && v<1) { for(int d=2;d<=1000000;++d) { double n=v*d; if(std::abs(n-std::round(n))<1e-7) return number(std::round(n))+"/"+std::to_string(d); } } return number(v); }
    double gpsCoord(size_t p) {
        double d=scalar(p,5),m=scalar(p+8,5),s=scalar(p+16,5);
        return d+m/60.0+s/3600.0;
    }
    void setText(const char* key,size_t entry,uint32_t n,uint16_t t) { size_t z=typeSize(t)*size_t(n); if(z>4) { size_t q; if(!dataLoc(u32(entry),n,t,q,z)) return; result[key]=ascii(q,z); } else if(has(entry,z)) result[key]=ascii(entry,z); }
    void setScalar(const char* key,size_t entry,uint32_t n,uint16_t t,bool exp=false) { size_t q,z; if(!n) return; z=typeSize(t)*size_t(n); if(z>4) {if(!dataLoc(u32(entry),n,t,q,z))return;} else q=entry; double v=scalar(q,t); result[key]=exp?exposure(v):number(v); }
    void visit(uint32_t off,int depth) {
        if(!off || depth>8) return;
        if(std::find(visited.begin(),visited.end(),off)!=visited.end()) return;
        visited.push_back(off);
        size_t p=base+size_t(off); if(p<base || !has(p,2)) {error="IFD out of bounds";return;}
        uint16_t count=u16(p); if(count>4096 || !has(p+2,size_t(count)*12+4)) {error="invalid IFD";return;}
        uint32_t exif=0,gps=0,next=0;
        for(uint16_t i=0;i<count;++i) {
            size_t e=p+2+size_t(i)*12; uint16_t tag=u16(e),type=u16(e+2); uint32_t n=u32(e+4),v=u32(e+8); size_t q,z;
            bool known = tag==0x010F||tag==0x0110||tag==0x0132||tag==0x0112||tag==0x013B||tag==0x8298||tag==0x8769||tag==0x8825||tag==0x829D||tag==0x829A||tag==0x8827||tag==0x920A||tag==0xA405||tag==0x9204||tag==0x9003||tag==0xA434;
            if(!known) continue;
            if(!typeSize(type) || n>std::numeric_limits<size_t>::max()/std::max<size_t>(1,typeSize(type))) { error="invalid type/count"; continue; }
            z=typeSize(type)*size_t(n); if(z>4 && !dataLoc(v,n,type,q,z)) continue;
            switch(tag) {
              case 0x010F:setText("Make",e+8,n,type);break; case 0x0110:setText("Model",e+8,n,type);break;
              case 0x0132:setText("DateTime",e+8,n,type);break; case 0x0112:setScalar("Orientation",e+8,n,type);break;
              case 0x013B:setText("Artist",e+8,n,type);break; case 0x8298:setText("Copyright",e+8,n,type);break;
              case 0x8769:if(n&&type==4)exif=v;break; case 0x8825:if(n&&type==4)gps=v;break;
              case 0x829D:setScalar("FNumber",e+8,n,type);break; case 0x829A:setScalar("ExposureTime",e+8,n,type,true);break;
              case 0x8827:setScalar("ISO",e+8,n,type);break; case 0x920A:setScalar("FocalLength",e+8,n,type);break;
              case 0xA405:setScalar("FocalLengthIn35mmFormat",e+8,n,type);break; case 0x9204:setScalar("ExposureBiasValue",e+8,n,type);break;
              case 0x9003:setText("DateTimeOriginal",e+8,n,type);break; case 0xA434:setText("LensModel",e+8,n,type);break;
              case 0x0001:case 0x0002:case 0x0003:case 0x0004: break;
            }
        }
        next=u32(p+2+size_t(count)*12);
        if(exif) visit(exif,depth+1);
        if(gps) visitGps(gps);
        if(next) visit(next,depth+1);
    }
    void visitGps(uint32_t off) {
        size_t p=base+size_t(off); if(p<base||!has(p,2)){error="GPS IFD out of bounds";return;}
        uint16_t count=u16(p); if(count>4096||!has(p+2,size_t(count)*12+4)){error="invalid GPS IFD";return;}
        std::string latRef,lonRef; size_t lat=0,lon=0;
        for(uint16_t i=0;i<count;++i) { size_t e=p+2+size_t(i)*12; uint16_t tag=u16(e),t=u16(e+2);uint32_t n=u32(e+4),v=u32(e+8);size_t q=0,z=0;
            if(tag==1&&t==2&&n) { z=typeSize(t)*size_t(n); if(z<=4)latRef=ascii(e+8,z); else if(dataLoc(v,n,t,q,z))latRef=ascii(q,z); }
            if(tag==3&&t==2&&n) { z=typeSize(t)*size_t(n); if(z<=4)lonRef=ascii(e+8,z); else if(dataLoc(v,n,t,q,z))lonRef=ascii(q,z); }
            if((tag==2||tag==4)&&t==5&&n>=3&&dataLoc(v,n,t,q,z)) { if(tag==2)lat=q; else lon=q; }
        }
        if(lat&&lon) { double a=gpsCoord(lat),o=gpsCoord(lon);if(!latRef.empty()&&latRef[0]=='S')a=-a;if(!lonRef.empty()&&lonRef[0]=='W')o=-o;result["GPSLatitude"]=number(a);result["GPSLongitude"]=number(o); }
    }
};
}

inline nlohmann::json parse(const std::vector<uint8_t>& bytes) {
    return detail::Reader(bytes).run();
}
// Returns the lowercase key used by the bundled camera logo assets, or an
// empty string when the make has no logo. Handles common EXIF vendor suffixes.
inline std::string camera_logo_key(const std::string& make) {
    auto trim=[](std::string s) { const auto first=s.find_first_not_of(" \t\r\n");
        return first==std::string::npos ? std::string{} : s.substr(first,s.find_last_not_of(" \t\r\n")-first+1); };
    std::string normalized=trim(make);
    std::transform(normalized.begin(),normalized.end(),normalized.begin(),[](unsigned char c){return char(std::tolower(c));});
    while(!normalized.empty() && (normalized.back()=='.' || normalized.back()==',')) normalized.pop_back();
    // Exact names plus common legal/vendor suffixes; don't match arbitrary
    // substrings, which could turn a phone make into a camera logo.
    static const std::array<const char*,13> brands={"canon","nikon","sony","fujifilm","hasselblad","leica","olympus","panasonic","pentax","ricoh","sigma","dji","songdian"};
    for(const char* brand:brands) {
        const std::string key(brand);
        if(normalized==key) return key;
        if(normalized.size()>key.size() && normalized.compare(0,key.size(),key)==0 &&
           std::isspace(static_cast<unsigned char>(normalized[key.size()]))) {
            std::string suffix=normalized.substr(key.size());
            while(!suffix.empty() && std::isspace(static_cast<unsigned char>(suffix.front()))) suffix.erase(suffix.begin());
            if(suffix=="corporation" || suffix=="corp." || suffix=="corp" || suffix=="co., ltd." || suffix=="co ltd" || suffix=="inc." || suffix=="inc") return key;
        }
    }
    return {};
}
inline std::pair<std::string,std::string> captions(const nlohmann::json& fields) {
    const auto value = [&](const char* key) -> std::string {
        if (!fields.contains(key) || fields[key].is_null()) return {};
        return fields[key].is_string() ? fields[key].get<std::string>() : fields[key].dump();
    };
    auto make = value("Make"), model = value("Model");
    auto trim = [](std::string s) { const auto first=s.find_first_not_of(" \t\r\n");
        return first==std::string::npos ? std::string{} : s.substr(first,s.find_last_not_of(" \t\r\n")-first+1); };
    make=trim(make); model=trim(model);
    const std::string logoKey=camera_logo_key(make);
    if(!model.empty() && !logoKey.empty()) {
        auto stripPrefix=[&](const std::string& prefix) {
            if(model.size()<prefix.size()) return false;
            for(size_t i=0;i<prefix.size();++i)
                if(std::tolower(static_cast<unsigned char>(model[i]))!=std::tolower(static_cast<unsigned char>(prefix[i]))) return false;
            if(model.size()>prefix.size() && !std::isspace(static_cast<unsigned char>(model[prefix.size()]))) return false;
            model.erase(0,prefix.size()); model=trim(model); return true;
        };
        while(stripPrefix(logoKey)) {}
    }
    std::string first=model.empty()?(logoKey.empty()?make:std::string{}):model;
    std::vector<std::string> parts;
    auto focal=value("FocalLengthIn35mmFormat"); if(focal.empty()||focal=="0") focal=value("FocalLength");
    if(!focal.empty()&&focal!="0") parts.push_back(focal+"mm");
    auto aperture=value("FNumber"); if(!aperture.empty()) parts.push_back("f/"+aperture);
    auto exposure=value("ExposureTime"); if(!exposure.empty())parts.push_back(exposure+"s");
    auto iso=value("ISO"); if(!iso.empty())parts.push_back("ISO"+iso);
    std::string second;for(const auto& part:parts){if(!second.empty())second+=' ';second+=part;}
    return {first,second};
}

// Repack metadata rather than embedding a source TIFF raster or thumbnail.
// All retained out-of-line data and IFD pointers are relocated in the output.
inline std::vector<uint8_t> output(const std::vector<uint8_t>& raw,uint32_t width,uint32_t height) {
    size_t base=std::string::npos;
    const auto tiff=[&](size_t p){return p<=raw.size()&&raw.size()-p>=8&&
        ((raw[p]=='I'&&raw[p+1]=='I'&&raw[p+2]==42&&raw[p+3]==0)||
         (raw[p]=='M'&&raw[p+1]=='M'&&raw[p+2]==0&&raw[p+3]==42));};
    if(tiff(0))base=0;
    else if(raw.size()>=6&&std::equal(raw.begin(),raw.begin()+6,"Exif\0\0")&&tiff(6))base=6;
    else if(raw.size()>=4) {const size_t o=(size_t(raw[0])<<24)|(size_t(raw[1])<<16)|(size_t(raw[2])<<8)|raw[3];
        if(tiff(o))base=o;else if(o<=raw.size()-4&&tiff(o+4))base=o+4;}
    if(base==std::string::npos) { if(raw.empty())return {}; throw std::runtime_error("Invalid source EXIF TIFF header"); }
    const bool le=raw[base]=='I';
    const auto u16=[&](size_t p)->uint16_t{if(p>raw.size()||raw.size()-p<2)throw std::runtime_error("Truncated EXIF");
        return le?uint16_t(raw[p]|uint16_t(raw[p+1])<<8):uint16_t(uint16_t(raw[p])<<8|raw[p+1]);};
    const auto u32=[&](size_t p)->uint32_t{if(p>raw.size()||raw.size()-p<4)throw std::runtime_error("Truncated EXIF");
        return le?uint32_t(raw[p])|uint32_t(raw[p+1])<<8|uint32_t(raw[p+2])<<16|uint32_t(raw[p+3])<<24:
                  uint32_t(raw[p])<<24|uint32_t(raw[p+1])<<16|uint32_t(raw[p+2])<<8|raw[p+3];};
    const auto typeSize=[](uint16_t t)->size_t {switch(t){case 1:case 2:case 6:case 7:return 1;case 3:case 8:return 2;
        case 4:case 9:case 11:return 4;case 5:case 10:case 12:return 8;default:return 0;}};
    struct Entry {uint16_t type;uint32_t count;std::vector<uint8_t> data;};
    using Directory=std::map<uint16_t,Entry>;
    std::array<Directory,3> dirs;
    auto read=[&](uint32_t offset,Directory& dir,bool root){if(!offset)return;
        const size_t p=base+size_t(offset);if(p<base)throw std::runtime_error("EXIF offset overflow");
        const uint16_t count=u16(p);if(count>4096||p>raw.size()||raw.size()-p<2+size_t(count)*12+4)throw std::runtime_error("Invalid EXIF directory");
        for(size_t i=0;i<count;++i){const size_t e=p+2+i*12;const uint16_t tag=u16(e),type=u16(e+2);const uint32_t n=u32(e+4);
            if(tag==0x927c||tag==0xa005||tag==0x8773||tag==0x02bc||tag==0x8649||tag==0x014a)continue;
            if(root&&(tag==0x0102||tag==0x0103||tag==0x0106||tag==0x0111||tag==0x0115||tag==0x0116||tag==0x0117||tag==0x011c||tag==0x013d||tag==0x0140||tag==0x0142||tag==0x0143||tag==0x0144||tag==0x0145||tag==0x0201||tag==0x0202||tag==0x013e||tag==0x013f))continue;
            const size_t size=typeSize(type);if(!size||n>raw.size()/size)continue;const size_t bytes=size*n;
            const size_t q=bytes<=4?e+8:base+size_t(u32(e+8));if(q<base||q>raw.size()||bytes>raw.size()-q)throw std::runtime_error("EXIF data outside source");
            dir[tag]={type,n,std::vector<uint8_t>(raw.begin()+q,raw.begin()+q+bytes)};
        }};
    read(u32(base+4),dirs[0],true);
    for(const auto [tag,index]:{std::pair<uint16_t,size_t>{0x8769,1},{0x8825,2}}){auto found=dirs[0].find(tag);
        if(found!=dirs[0].end()){if(found->second.type==4&&found->second.count==1) {const auto& b=found->second.data;
            const uint32_t off=le?uint32_t(b[0])|uint32_t(b[1])<<8|uint32_t(b[2])<<16|uint32_t(b[3])<<24:
                uint32_t(b[0])<<24|uint32_t(b[1])<<16|uint32_t(b[2])<<8|b[3];read(off,dirs[index],false);}
            dirs[0].erase(found);}}
    const auto number=[&](uint32_t n,uint16_t type)->Entry{std::vector<uint8_t>b(type==3?2:4);for(size_t i=0;i<b.size();++i)b[i]=uint8_t(n>>(8*(le?i:b.size()-1-i)));return {type,1,std::move(b)};};
    dirs[0][0x0100]=number(width,4);dirs[0][0x0101]=number(height,4);dirs[0][0x0112]=number(1,3);
    dirs[1][0xa002]=number(width,4);dirs[1][0xa003]=number(height,4);dirs[1][0xa001]=number(65535,3);
    dirs[0][0x8769]=number(0,4);if(!dirs[2].empty())dirs[0][0x8825]=number(0,4);
    std::vector<uint8_t> out{uint8_t(le?'I':'M'),uint8_t(le?'I':'M'),uint8_t(le?42:0),uint8_t(le?0:42),0,0,0,0};
    auto put16=[&](size_t p,uint16_t v){out[p]=uint8_t(v>>(le?0:8));out[p+1]=uint8_t(v>>(le?8:0));};
    auto put32=[&](size_t p,uint32_t v){for(size_t i=0;i<4;++i)out[p+i]=uint8_t(v>>(8*(le?i:3-i)));};
    std::array<uint32_t,3> locations{};
    for(size_t d=0;d<3;++d){if(dirs[d].empty())continue;locations[d]=uint32_t(out.size());out.resize(out.size()+2+dirs[d].size()*12+4);put16(locations[d],uint16_t(dirs[d].size()));}
    put32(4,locations[0]);
    for(size_t d=0;d<3;++d){size_t i=0;for(const auto& [tag,e]:dirs[d]){const size_t p=locations[d]+2+i++*12;put16(p,tag);put16(p+2,e.type);put32(p+4,e.count);
        if(d==0&&(tag==0x8769||tag==0x8825)){put32(p+8,locations[tag==0x8769?1:2]);continue;}
        if(e.data.size()<=4)std::copy(e.data.begin(),e.data.end(),out.begin()+p+8);
        else{if(out.size()&1)out.push_back(0);const size_t o=out.size();if(o>UINT32_MAX-e.data.size())throw std::runtime_error("EXIF too large");
            out.insert(out.end(),e.data.begin(),e.data.end());put32(p+8,uint32_t(o));}}}
    return out;
}
} // namespace hdrbridge::frame_exif
