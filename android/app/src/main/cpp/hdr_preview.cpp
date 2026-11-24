#include "hdr_preview.h"
#include "hdrbridge_core.h"
#include "hdr_preview_tiles.h"
#include <android/native_window.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <android/data_space.h>
#include <android/hardware_buffer.h>
#include <android/surface_control.h>
#include <dlfcn.h>
#include <poll.h>
#include <unistd.h>
#include <array>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>

namespace hdrbridge::android_preview {
namespace {
constexpr EGLint kComponentType = 0x3339, kFloat = 0x333B;
constexpr EGLint kColorspace = 0x309D, kScRgbLinear = 0x3350, kPq = 0x3340;
// EGL's default display is shared across preview sessions. Keep it initialized
// for the process lifetime so one session cannot terminate another's context.
EGLDisplay sharedDisplay() {
  static EGLDisplay display = [] {
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    return d != EGL_NO_DISPLAY && eglInitialize(d, nullptr, nullptr) ? d : EGL_NO_DISPLAY;
  }();
  return display;
}
// Resolve newer Android entry points at runtime so the library can load on API 26.
struct SurfaceApi {
  void* library = nullptr;
  decltype(&AHardwareBuffer_allocate) allocate = nullptr;
  decltype(&AHardwareBuffer_release) releaseBuffer = nullptr;
  ASurfaceControl* (*create)(ANativeWindow*, const char*) = nullptr;
  void (*acquire)(ASurfaceControl*) = nullptr;
  void (*release)(ASurfaceControl*) = nullptr;
  ASurfaceTransaction* (*transaction)() = nullptr;
  void (*destroyTransaction)(ASurfaceTransaction*) = nullptr;
  void (*apply)(ASurfaceTransaction*) = nullptr;
  void (*reparent)(ASurfaceTransaction*, ASurfaceControl*, ASurfaceControl*) = nullptr;
  void (*visibility)(ASurfaceTransaction*, ASurfaceControl*, int8_t) = nullptr;
  void (*zorder)(ASurfaceTransaction*, ASurfaceControl*, int32_t) = nullptr;
  void (*buffer)(ASurfaceTransaction*, ASurfaceControl*, AHardwareBuffer*, int) = nullptr;
  void (*transparency)(ASurfaceTransaction*, ASurfaceControl*, int8_t) = nullptr;
  void (*dataspace)(ASurfaceTransaction*, ASurfaceControl*, ADataSpace) = nullptr;
  void (*geometry)(ASurfaceTransaction*, ASurfaceControl*, const ARect&, const ARect&, int32_t) = nullptr;
  void (*brightness)(ASurfaceTransaction*, ASurfaceControl*, float, float) = nullptr;
  void (*completion)(ASurfaceTransaction*, void*, ASurfaceTransaction_OnComplete) = nullptr;
  int (*previousFence)(ASurfaceTransactionStats*, ASurfaceControl*) = nullptr;
  PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC clientBuffer = nullptr;
  PFNEGLCREATEIMAGEKHRPROC createImage = nullptr;
  PFNEGLDESTROYIMAGEKHRPROC destroyImage = nullptr;
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC imageTexture = nullptr;
  PFNEGLCREATESYNCKHRPROC createSync = nullptr;
  PFNEGLDESTROYSYNCKHRPROC destroySync = nullptr;
  PFNEGLDUPNATIVEFENCEFDANDROIDPROC dupFence = nullptr;
  SurfaceApi() {
    library = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    if (!library) return;
#define LOAD(member, symbol) member = reinterpret_cast<decltype(member)>(dlsym(library, #symbol))
    LOAD(allocate, AHardwareBuffer_allocate); LOAD(releaseBuffer, AHardwareBuffer_release);
    LOAD(create, ASurfaceControl_createFromWindow); LOAD(acquire, ASurfaceControl_acquire); LOAD(release, ASurfaceControl_release);
    LOAD(transaction, ASurfaceTransaction_create); LOAD(destroyTransaction, ASurfaceTransaction_delete); LOAD(apply, ASurfaceTransaction_apply);
    LOAD(reparent, ASurfaceTransaction_reparent); LOAD(visibility, ASurfaceTransaction_setVisibility); LOAD(zorder, ASurfaceTransaction_setZOrder);
    LOAD(buffer, ASurfaceTransaction_setBuffer); LOAD(transparency, ASurfaceTransaction_setBufferTransparency);
    LOAD(dataspace, ASurfaceTransaction_setBufferDataSpace); LOAD(geometry, ASurfaceTransaction_setGeometry);
    LOAD(brightness, ASurfaceTransaction_setExtendedRangeBrightness); LOAD(completion, ASurfaceTransaction_setOnComplete);
    LOAD(previousFence, ASurfaceTransactionStats_getPreviousReleaseFenceFd);
#undef LOAD
#define EGL_LOAD(member, type, symbol) member = reinterpret_cast<type>(eglGetProcAddress(symbol))
    EGL_LOAD(clientBuffer, PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC, "eglGetNativeClientBufferANDROID");
    EGL_LOAD(createImage, PFNEGLCREATEIMAGEKHRPROC, "eglCreateImageKHR"); EGL_LOAD(destroyImage, PFNEGLDESTROYIMAGEKHRPROC, "eglDestroyImageKHR");
    EGL_LOAD(imageTexture, PFNGLEGLIMAGETARGETTEXTURE2DOESPROC, "glEGLImageTargetTexture2DOES");
    EGL_LOAD(createSync, PFNEGLCREATESYNCKHRPROC, "eglCreateSyncKHR"); EGL_LOAD(destroySync, PFNEGLDESTROYSYNCKHRPROC, "eglDestroySyncKHR");
    EGL_LOAD(dupFence, PFNEGLDUPNATIVEFENCEFDANDROIDPROC, "eglDupNativeFenceFDANDROID");
#undef EGL_LOAD
  }
  bool ready() const { return allocate && releaseBuffer && create && acquire && release && transaction && destroyTransaction && apply && reparent && visibility && zorder && buffer && transparency && dataspace && geometry && brightness && completion && previousFence && clientBuffer && createImage && destroyImage && imageTexture && createSync && destroySync && dupFence; }
};
SurfaceApi& surfaceApi() { static SurfaceApi api; return api; }
struct RingState {
  std::mutex mutex;
  std::array<bool, 3> busy{};
  std::array<int, 3> fence{{-1,-1,-1}};
  int current = -1;
  ~RingState() { for (int fd : fence) if (fd >= 0) close(fd); }
};
struct Completion { std::shared_ptr<RingState> ring; ASurfaceControl* surface; int previous; };
void completed(void* data, ASurfaceTransactionStats* stats) {
  std::unique_ptr<Completion> c(static_cast<Completion*>(data)); auto& api=surfaceApi();
  if (stats && c->previous >= 0) {
    int fd = api.previousFence(stats, c->surface);
    std::lock_guard<std::mutex> lock(c->ring->mutex);
    c->ring->fence[c->previous] = fd;
    c->ring->busy[c->previous] = false;
  }
  api.release(c->surface);
}
bool extension(const char* list, const char* name) {
  if (!list) return false;
  const size_t n = std::strlen(name);
  for (const char* p = list; (p = std::strstr(p, name)); p += n)
    if ((p == list || p[-1] == ' ') && (p[n] == 0 || p[n] == ' ')) return true;
  return false;
}
GLuint shader(GLenum type, const char* source) {
  GLuint s = glCreateShader(type); glShaderSource(s, 1, &source, nullptr); glCompileShader(s);
  GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) { glDeleteShader(s); return 0; } return s;
}
float half(uint16_t h) {
  unsigned e = (h >> 10) & 31, m = h & 1023;
  float v = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? 0.f : std::ldexp(float(m + 1024), int(e) - 25);
  return (h & 0x8000) ? -v : v;
}
}
struct Renderer::Impl {
  PreviewImage image;
  std::vector<preview::MipLevel> mipLevels;
  float peak = 1;
  EGLDisplay display = EGL_NO_DISPLAY; EGLContext context = EGL_NO_CONTEXT; EGLSurface surface = EGL_NO_SURFACE;
  GLuint program = 0, vao = 0;
  uint32_t textureLimit = 0;
  struct Texture { preview::Tile tile; GLuint name = 0; };
  struct Level {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<preview::TileSpan> cols;
    std::vector<preview::TileSpan> rows;
    std::vector<Texture> textures;
  };
  std::vector<Level> levels;
  std::vector<preview::LevelExtent> levelExtents;
  int mode = 0, ringWidth = 0, ringHeight = 0;
  ASurfaceControl* child = nullptr;
  struct Slot { AHardwareBuffer* buffer=nullptr; EGLImageKHR image=EGL_NO_IMAGE_KHR; GLuint texture=0, framebuffer=0; };
  std::array<Slot, 3> slots{};
  std::shared_ptr<RingState> ring;
  void clearTextures(bool contextCurrent = true) {
    if (contextCurrent) {
      for (const auto& level : levels) {
        for (const auto& texture : level.textures) glDeleteTextures(1, &texture.name);
      }
    }
    levels.clear();
    levelExtents.clear();
  }
  bool dropExtraMipLevels(bool contextCurrent = true) {
    if (levels.size() <= 1u) return false;
    if (contextCurrent) {
      for (size_t i = 1u; i < levels.size(); ++i) {
        for (const auto& texture : levels[i].textures) glDeleteTextures(1, &texture.name);
      }
    }
    levels.resize(1u);
    if (levelExtents.size() > 1u) levelExtents.resize(1u);
    return true;
  }
  bool uploadLevel(uint32_t w, uint32_t h, const uint16_t* rgbaFp16, uint32_t maxLimit, Level& out) {
    while (glGetError() != GL_NO_ERROR) {}
    glActiveTexture(GL_TEXTURE0);
    out.width = w;
    out.height = h;
    out.cols = preview::split_texture_axis(w, maxLimit);
    out.rows = preview::split_texture_axis(h, maxLimit);
    out.textures.reserve(out.cols.size() * out.rows.size());
    glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(w));
    for (const auto& y : out.rows) {
      for (const auto& x : out.cols) {
        const preview::Tile tile{x, y};
        auto& texture = out.textures.emplace_back(Texture{tile});
        glGenTextures(1, &texture.name);
        glBindTexture(GL_TEXTURE_2D, texture.name);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        const auto* pixels = rgbaFp16 +
            (static_cast<size_t>(tile.y.sample_offset) * w + tile.x.sample_offset) * 4u;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, tile.x.sample_length, tile.y.sample_length,
                     0, GL_RGBA, GL_HALF_FLOAT, pixels);
        if (glGetError() != GL_NO_ERROR) {
          glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
          for (const auto& partial : out.textures) glDeleteTextures(1, &partial.name);
          out.textures.clear();
          return false;
        }
      }
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    return true;
  }
  bool uploadTextures() {
    GLint limit = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &limit);
    if (limit <= 0) return false;
    const uint32_t maxLimit = std::min(textureLimit, static_cast<uint32_t>(limit));
    levels.clear();
    levelExtents.clear();
    levels.reserve(1u + mipLevels.size());
    levelExtents.reserve(1u + mipLevels.size());
    if (!uploadLevel(image.width, image.height, image.rgba_fp16.data(), maxLimit, levels.emplace_back())) {
      clearTextures();
      return false;
    }
    levelExtents.push_back({image.width, image.height});
    if (mipLevels.empty() && std::max(image.width, image.height) > 64u) {
      try {
        mipLevels = preview::build_2x_mip_levels(image.width, image.height, image.rgba_fp16);
      } catch (const std::exception&) {
        mipLevels.clear();
      }
    }
    for (const auto& mip : mipLevels) {
      try {
        Level mipLevel;
        if (!uploadLevel(mip.width, mip.height, mip.rgba_fp16.data(), maxLimit, mipLevel)) {
          break;
        }
        levels.push_back(std::move(mipLevel));
        levelExtents.push_back({mip.width, mip.height});
      } catch (const std::exception&) {
        break;
      }
    }
    std::vector<preview::MipLevel>().swap(mipLevels);
    return true;
  }
  void clearSlots(bool contextCurrent = true) {
    auto& api=surfaceApi();
    for(auto& s: slots) {
      if(contextCurrent && s.framebuffer) glDeleteFramebuffers(1,&s.framebuffer);
      if(contextCurrent && s.texture) glDeleteTextures(1,&s.texture);
      if(s.image!=EGL_NO_IMAGE_KHR) api.destroyImage(display,s.image);
      if(s.buffer) api.releaseBuffer(s.buffer);
      s=Slot{};
    }
    ring.reset(); ringWidth=ringHeight=0;
  }
  bool makeSlots(int w,int h) {
    if(w==ringWidth && h==ringHeight) return true;
    clearSlots(); auto& api=surfaceApi();
    AHardwareBuffer_Desc desc{}; desc.width=w; desc.height=h; desc.layers=1;
    desc.format=AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM;
    desc.usage=AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE|AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER|AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY;
    for(auto& s: slots) {
      if(api.allocate(&desc,&s.buffer)!=0 || !s.buffer) { clearSlots(); return false; }
      EGLClientBuffer b=api.clientBuffer(s.buffer);
      const EGLint attrs[]={EGL_IMAGE_PRESERVED_KHR,EGL_TRUE,EGL_NONE};
      if(!b || (s.image=api.createImage(display,EGL_NO_CONTEXT,EGL_NATIVE_BUFFER_ANDROID,b,attrs))==EGL_NO_IMAGE_KHR) { clearSlots(); return false; }
      glGenTextures(1,&s.texture); glBindTexture(GL_TEXTURE_2D,s.texture); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
      api.imageTexture(GL_TEXTURE_2D,static_cast<GLeglImageOES>(s.image));
      glGenFramebuffers(1,&s.framebuffer); glBindFramebuffer(GL_FRAMEBUFFER,s.framebuffer); glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,s.texture,0);
      if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE || glGetError()!=GL_NO_ERROR) { glBindFramebuffer(GL_FRAMEBUFFER,0); clearSlots(); return false; }
    }
    glBindFramebuffer(GL_FRAMEBUFFER,0); ring=std::make_shared<RingState>(); ringWidth=w; ringHeight=h; return true;
  }
};
Renderer::Renderer() : impl_(std::make_unique<Impl>()) {}
Renderer::~Renderer() { detach(); }
int Renderer::maxTextureSize() {
  EGLDisplay d = sharedDisplay(); if (d == EGL_NO_DISPLAY) return 0;
  EGLDisplay oldDisplay = eglGetCurrentDisplay();
  EGLContext oldContext = eglGetCurrentContext();
  EGLSurface oldDraw = eglGetCurrentSurface(EGL_DRAW), oldRead = eglGetCurrentSurface(EGL_READ);
  EGLint attrs[] = {EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT_KHR,EGL_NONE}; EGLConfig cfg; EGLint n=0;
  int result=0;
  if (eglChooseConfig(d,attrs,&cfg,1,&n) && n) {
    EGLint ca[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE}; EGLContext c=eglCreateContext(d,cfg,EGL_NO_CONTEXT,ca);
    EGLint pa[]={EGL_WIDTH,1,EGL_HEIGHT,1,EGL_NONE}; EGLSurface s=eglCreatePbufferSurface(d,cfg,pa);
    if(c!=EGL_NO_CONTEXT && s!=EGL_NO_SURFACE && eglMakeCurrent(d,s,s,c)) { glGetIntegerv(GL_MAX_TEXTURE_SIZE,&result); eglMakeCurrent(d,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT); }
    if(s!=EGL_NO_SURFACE) eglDestroySurface(d,s); if(c!=EGL_NO_CONTEXT) eglDestroyContext(d,c);
  }
  if (oldDisplay != EGL_NO_DISPLAY && oldContext != EGL_NO_CONTEXT)
    eglMakeCurrent(oldDisplay, oldDraw, oldRead, oldContext);
  return result;
}
bool Renderer::decode(const std::string& path, uint32_t limit, std::string& error) {
  try {
    auto image = decode_preview(path, std::numeric_limits<uint32_t>::max());
    if (!image.width || !image.height || image.rgba_fp16.empty()) { error="Image decode produced no pixels"; return false; }
    // Positive finite IEEE half values have the same ordering as their bit patterns.
    // Scan all channels exactly, converting only the maximum instead of every sample.
    uint16_t maximum = 0;
    for (size_t i = 0; i + 3 < image.rgba_fp16.size(); i += 4) {
      for (size_t c = 0; c < 3; ++c) {
        const auto value = image.rgba_fp16[i + c];
        if (value < 0x7c00u) maximum = std::max(maximum, value);
      }
    }
    const float peak = std::max(1.0f, half(maximum));
    std::vector<preview::MipLevel> mipLevels;
    try {
      mipLevels = preview::build_2x_mip_levels(image.width, image.height, image.rgba_fp16);
    } catch (const std::exception&) {
      mipLevels.clear();
    }
    impl_->image=std::move(image); impl_->mipLevels=std::move(mipLevels); impl_->peak=std::clamp(peak / (203.f / 80.f),1.f,16.f); impl_->textureLimit=limit; return true;
  } catch(const std::exception& e) { error=e.what(); return false; }
}
uint32_t Renderer::width() const { return impl_->image.width; }
uint32_t Renderer::height() const { return impl_->image.height; }
float Renderer::peakRatio() const { return impl_->peak; }
int Renderer::attach(ANativeWindow* window, std::string& error) {
  detach(); auto& p=*impl_; if(!window) { error="No preview surface"; return 0; }
  auto tryMode = [&](int mode) -> bool {
    p.display=sharedDisplay();
    if(p.display==EGL_NO_DISPLAY) return false;
    const char* ex=eglQueryString(p.display,EGL_EXTENSIONS); auto& api=surfaceApi();
    if(mode==1 && (!api.ready() || !extension(ex,"EGL_ANDROID_get_native_client_buffer") || !extension(ex,"EGL_KHR_image_base") || !extension(ex,"EGL_ANDROID_native_fence_sync"))) return false;
    if(mode==2 && !extension(ex,"EGL_EXT_gl_colorspace_bt2020_pq")) return false;
    if(mode==3 && (!extension(ex,"EGL_EXT_pixel_format_float") || !extension(ex,"EGL_EXT_gl_colorspace_scrgb_linear"))) return false;
    EGLint scAttrs[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT_KHR,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
    EGLint pqAttrs[]={EGL_SURFACE_TYPE,EGL_WINDOW_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT_KHR,EGL_RED_SIZE,10,EGL_GREEN_SIZE,10,EGL_BLUE_SIZE,10,EGL_ALPHA_SIZE,2,EGL_NONE};
    EGLint floatAttrs[]={EGL_SURFACE_TYPE,EGL_WINDOW_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT_KHR,EGL_RED_SIZE,16,EGL_GREEN_SIZE,16,EGL_BLUE_SIZE,16,EGL_ALPHA_SIZE,16,kComponentType,kFloat,EGL_NONE};
    EGLConfig cfg; EGLint n=0;
    if(!eglChooseConfig(p.display,mode==1?scAttrs:mode==2?pqAttrs:floatAttrs,&cfg,1,&n) || !n) return false;
    EGLint ca[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE}; p.context=eglCreateContext(p.display,cfg,EGL_NO_CONTEXT,ca);
    if(p.context==EGL_NO_CONTEXT) return false;
    if(mode==1) {
      p.child=api.create(window,"HdrBridge HDR"); EGLint pa[]={EGL_WIDTH,1,EGL_HEIGHT,1,EGL_NONE}; p.surface=eglCreatePbufferSurface(p.display,cfg,pa);
      if(!p.child) return false;
    } else {
      EGLint format=0; eglGetConfigAttrib(p.display,cfg,EGL_NATIVE_VISUAL_ID,&format); ANativeWindow_setBuffersGeometry(window,0,0,format);
      EGLint sa[]={kColorspace,mode==2?kPq:kScRgbLinear,EGL_NONE}; p.surface=eglCreateWindowSurface(p.display,cfg,window,sa);
    }
    if(p.surface==EGL_NO_SURFACE || !eglMakeCurrent(p.display,p.surface,p.surface,p.context)) return false;
    const char* vs=R"(#version 300 es
      precision highp float; uniform vec4 bounds; uniform vec4 sampleRect0; uniform vec4 sampleRect1; uniform int flip; out vec2 uv0; out vec2 uv1;
      void main(){
        bool right=(gl_VertexID&1)==1, top=gl_VertexID>=2;
        bool sampleTop=(top!=(flip==1));
        uv0=vec2(right?sampleRect0.z:sampleRect0.x,sampleTop?sampleRect0.y:sampleRect0.w);
        uv1=vec2(right?sampleRect1.z:sampleRect1.x,sampleTop?sampleRect1.y:sampleRect1.w);
        gl_Position=vec4(right?bounds.z:bounds.x,top?bounds.w:bounds.y,0,1);
      })";
    const char* fs=R"(#version 300 es
      precision highp float; uniform sampler2D image0; uniform sampler2D image1; uniform float lodBlend; uniform int mode; uniform float headroom; uniform float peak; in vec2 uv0; in vec2 uv1; out vec4 color;
      void main(){
        vec3 c=texture(image0,uv0).rgb;
        if(lodBlend>0.) {
          c=mix(c,texture(image1,uv1).rgb,lodBlend);
        }
        const mat3 m=mat3(0.627404,0.069097,0.016391, 0.329283,0.919541,0.088013, 0.043313,0.011362,0.895595);
        if(mode==1) {
          vec3 r=max(m*c,0.)*(80./203.);
          float y=dot(r,vec3(.2627,.6780,.0593));
          if(y>1.&&peak>headroom+.001) {
            float hdrSpan=peak-1.;
            float availSpan=max(headroom-1.,0.);
            float e=y-1.;
            float ym=availSpan<=.001?1.:1.+e/(1.+e*(hdrSpan-availSpan)/(availSpan*hdrSpan));
            r*=ym/y;
          }
          vec3 x=clamp(r/headroom,0.,1.);
          c=mix(1.055*pow(max(x,vec3(1e-6)),vec3(1./2.4))-.055,12.92*x,lessThanEqual(x,vec3(.0031308)));
        }
        else if(mode==2) { vec3 x=pow(clamp(max(m*c,0.)*(80./10000.),0.,1.),vec3(.1593017578125)); c=pow((.8359375+18.8515625*x)/(1.+18.6875*x),vec3(78.84375)); }
        else c*=80./203.; color=vec4(c,1.); })";
    GLuint v=shader(GL_VERTEX_SHADER,vs), f=shader(GL_FRAGMENT_SHADER,fs);
    if(!v || !f) { if(v) glDeleteShader(v); if(f) glDeleteShader(f); return false; }
    p.program=glCreateProgram(); glAttachShader(p.program,v); glAttachShader(p.program,f); glLinkProgram(p.program); glDeleteShader(v); glDeleteShader(f); GLint ok=0; glGetProgramiv(p.program,GL_LINK_STATUS,&ok); if(!ok) return false;
    glGenVertexArrays(1,&p.vao);
    if(mode==1 && !p.makeSlots(64,64)) return false;
    p.mode=mode; return true;
  };
  for(int mode: {1,2,3}) { if(tryMode(mode)) { error.clear(); return mode; } detach(); }
  error="No supported HDR SurfaceControl, PQ, or linear floating-point window configuration"; return 0;
}
FrameResult Renderer::render(int w,int h,float scale,float x,float y,float displayHeadroom) {
  auto& p=*impl_; if(p.surface==EGL_NO_SURFACE || !p.image.width || w<=0 || h<=0 || !std::isfinite(scale) || scale<=0 || !std::isfinite(x) || !std::isfinite(y)) return FrameResult::Busy;
  if(!eglMakeCurrent(p.display,p.surface,p.surface,p.context)) return FrameResult::Busy;
  int index=-1; auto& api=surfaceApi();
  if(p.mode==1) {
    if(!p.makeSlots(w,h)) {
      if(!p.dropExtraMipLevels(true)) return FrameResult::Busy;
      glFinish();
      if(!p.makeSlots(w,h)) return FrameResult::Busy;
    }
  }
  glActiveTexture(GL_TEXTURE0);
  if(p.levels.empty() && !p.uploadTextures()) return FrameResult::Unavailable;
  if(p.mode==1) {
    std::lock_guard<std::mutex> lock(p.ring->mutex);
    for(int i=0;i<3;++i) {
      if(p.ring->busy[i]) continue;
      int fd=p.ring->fence[i];
      if(fd>=0) { pollfd f{fd,POLLIN,0}; if(poll(&f,1,0)<=0 || !(f.revents&POLLIN)) continue; close(fd); p.ring->fence[i]=-1; }
      index=i; break;
    }
    if(index<0) return FrameResult::Busy;
    glBindFramebuffer(GL_FRAMEBUFFER,p.slots[index].framebuffer);
  } else glBindFramebuffer(GL_FRAMEBUFFER,0);
  glViewport(0,0,w,h); glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT); glUseProgram(p.program);
  glUniform1i(glGetUniformLocation(p.program,"image0"),0);
  glUniform1i(glGetUniformLocation(p.program,"image1"),1);
  glUniform1i(glGetUniformLocation(p.program,"mode"),p.mode); glUniform1i(glGetUniformLocation(p.program,"flip"),p.mode==1);
  float desired=std::clamp(p.peak,1.f,16.f);
  float headroom=(displayHeadroom>0.f && std::isfinite(displayHeadroom)) ? std::clamp(displayHeadroom,1.f,desired) : desired;
  glUniform1f(glGetUniformLocation(p.program,"headroom"),headroom); glUniform1f(glGetUniformLocation(p.program,"peak"),desired);
  glBindVertexArray(p.vao);
  const auto selection = preview::select_trilinear_levels(
      p.image.width, p.image.height, scale, p.levelExtents);
  const auto& level0 = p.levels[selection.level0];
  const auto& level1 = p.levels[selection.level1];
  const auto xSpans = preview::trilinear_axis_spans(
      p.image.width, level0.width, level0.cols, level1.width, level1.cols, selection.blend);
  const auto ySpans = preview::trilinear_axis_spans(
      p.image.height, level0.height, level0.rows, level1.height, level1.rows, selection.blend);
  const double rx0 = static_cast<double>(level0.width) / p.image.width;
  const double ry0 = static_cast<double>(level0.height) / p.image.height;
  const double rx1 = static_cast<double>(level1.width) / p.image.width;
  const double ry1 = static_cast<double>(level1.height) / p.image.height;
  glUniform1f(glGetUniformLocation(p.program, "lodBlend"), selection.blend);
  const GLint boundsUniform = glGetUniformLocation(p.program, "bounds");
  const GLint sampleRect0Uniform = glGetUniformLocation(p.program, "sampleRect0");
  const GLint sampleRect1Uniform = glGetUniformLocation(p.program, "sampleRect1");
  for (const auto& ry : ySpans) {
    const float top = 1 - 2 * static_cast<float>(y + ry.base_start * scale) / h;
    const float bottom = 1 - 2 * static_cast<float>(y + ry.base_end * scale) / h;
    if (top <= -1 || bottom >= 1) continue;
    for (const auto& rx : xSpans) {
      const float left = 2 * static_cast<float>(x + rx.base_start * scale) / w - 1;
      const float right = 2 * static_cast<float>(x + rx.base_end * scale) / w - 1;
      if (right <= -1 || left >= 1) continue;
      const auto& tex0 = level0.textures[ry.tile0 * level0.cols.size() + rx.tile0];
      const auto& tex1 = level1.textures[ry.tile1 * level1.cols.size() + rx.tile1];
      glUniform4f(boundsUniform, left, p.mode==1 ? -top : bottom, right, p.mode==1 ? -bottom : top);
      const float u0 = static_cast<float>(rx.base_start * rx0 - tex0.tile.x.sample_offset);
      const float v0 = static_cast<float>(ry.base_start * ry0 - tex0.tile.y.sample_offset);
      const float u0End = static_cast<float>(rx.base_end * rx0 - tex0.tile.x.sample_offset);
      const float v0End = static_cast<float>(ry.base_end * ry0 - tex0.tile.y.sample_offset);
      glUniform4f(sampleRect0Uniform,
                  u0 / tex0.tile.x.sample_length, v0 / tex0.tile.y.sample_length,
                  u0End / tex0.tile.x.sample_length, v0End / tex0.tile.y.sample_length);
      const float u1 = static_cast<float>(rx.base_start * rx1 - tex1.tile.x.sample_offset);
      const float v1 = static_cast<float>(ry.base_start * ry1 - tex1.tile.y.sample_offset);
      const float u1End = static_cast<float>(rx.base_end * rx1 - tex1.tile.x.sample_offset);
      const float v1End = static_cast<float>(ry.base_end * ry1 - tex1.tile.y.sample_offset);
      glUniform4f(sampleRect1Uniform,
                  u1 / tex1.tile.x.sample_length, v1 / tex1.tile.y.sample_length,
                  u1End / tex1.tile.x.sample_length, v1End / tex1.tile.y.sample_length);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, tex0.name);
      glActiveTexture(GL_TEXTURE1);
      glBindTexture(GL_TEXTURE_2D, tex1.name);
      glDrawArrays(GL_TRIANGLE_STRIP,0,4);
    }
  }
  glActiveTexture(GL_TEXTURE0);
  if(p.mode!=1) return eglSwapBuffers(p.display,p.surface) ? FrameResult::Drawn : FrameResult::Busy;
  glBindFramebuffer(GL_FRAMEBUFFER,0);
  ASurfaceTransaction* tx=api.transaction(); if(!tx) { glFinish(); return FrameResult::Busy; }
  EGLSyncKHR sync=api.createSync(p.display,EGL_SYNC_NATIVE_FENCE_ANDROID,nullptr); int fence=-1;
  if(sync!=EGL_NO_SYNC_KHR) { glFlush(); fence=api.dupFence(p.display,sync); api.destroySync(p.display,sync); }
  if(fence<0) {
    glFinish();
    if(glGetError()!=GL_NO_ERROR) { api.destroyTransaction(tx); return FrameResult::Busy; }
  }
  auto c=std::make_unique<Completion>(); c->ring=p.ring; c->surface=p.child; api.acquire(p.child);
  { std::lock_guard<std::mutex> lock(p.ring->mutex); c->previous=p.ring->current; p.ring->current=index; p.ring->busy[index]=true; }
  ARect bounds{0,0,w,h}; api.visibility(tx,p.child,ASURFACE_TRANSACTION_VISIBILITY_SHOW); api.zorder(tx,p.child,1);
  api.buffer(tx,p.child,p.slots[index].buffer,fence); api.transparency(tx,p.child,ASURFACE_TRANSACTION_TRANSPARENCY_OPAQUE);
  api.dataspace(tx,p.child,static_cast<ADataSpace>(ADATASPACE_STANDARD_BT2020|ADATASPACE_TRANSFER_SRGB|ADATASPACE_RANGE_EXTENDED)); api.geometry(tx,p.child,bounds,bounds,0); api.brightness(tx,p.child,headroom,desired);
  api.completion(tx,c.release(),completed); api.apply(tx); api.destroyTransaction(tx); return FrameResult::Drawn;
}
void Renderer::detach() {
  auto& p=*impl_; auto& api=surfaceApi();
  if(p.child) { if(auto* tx=api.transaction()) { api.visibility(tx,p.child,ASURFACE_TRANSACTION_VISIBILITY_HIDE); api.reparent(tx,p.child,nullptr); api.apply(tx); api.destroyTransaction(tx); } api.release(p.child); p.child=nullptr; }
  if (p.display != EGL_NO_DISPLAY) {
    const bool current = p.context != EGL_NO_CONTEXT && p.surface != EGL_NO_SURFACE &&
        eglMakeCurrent(p.display, p.surface, p.surface, p.context);
    if (current) glFinish();
    // CPU-owned AHardwareBuffer/EGLImage references must also be released after context loss.
    p.clearSlots(current);
    p.clearTextures(current);
    if (current) {
      if (p.program) glDeleteProgram(p.program);
      if (p.vao) glDeleteVertexArrays(1, &p.vao);
    }
    eglMakeCurrent(p.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (p.surface != EGL_NO_SURFACE) eglDestroySurface(p.display, p.surface);
    if (p.context != EGL_NO_CONTEXT) eglDestroyContext(p.display, p.context);
  }
  p.display=EGL_NO_DISPLAY; p.surface=EGL_NO_SURFACE; p.context=EGL_NO_CONTEXT; p.program=p.vao=0; p.mode=0;
}
}
