#include "hdr_preview.h"
#include "hdrbridge_core.h"
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
  float peak = 1;
  EGLDisplay display = EGL_NO_DISPLAY; EGLContext context = EGL_NO_CONTEXT; EGLSurface surface = EGL_NO_SURFACE;
  GLuint texture = 0, program = 0, vao = 0;
  bool uploaded = false;
  int mode = 0, ringWidth = 0, ringHeight = 0;
  ASurfaceControl* child = nullptr;
  struct Slot { AHardwareBuffer* buffer=nullptr; EGLImageKHR image=EGL_NO_IMAGE_KHR; GLuint texture=0, framebuffer=0; };
  std::array<Slot, 3> slots{};
  std::shared_ptr<RingState> ring;
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
  return std::min(result,16384);
}
bool Renderer::decode(const std::string& path, uint32_t limit, std::string& error) {
  try {
    auto info = inspect(path);
    if (info.width > limit || info.height > limit) { error="Image exceeds the maximum HDR texture size"; return false; }
    auto image = decode_preview(path, limit); if (!image.width || !image.height || image.rgba_fp16.empty()) { error="Image decode produced no pixels"; return false; }
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
    impl_->image=std::move(image); impl_->peak=std::clamp(peak / (203.f / 80.f),1.f,16.f); impl_->uploaded=false; return true;
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
      precision highp float; uniform vec4 transform; uniform int flip; out vec2 uv;
      void main(){ vec2 p=vec2((gl_VertexID&1)==1?1.:-1.,gl_VertexID>=2?1.:-1.); uv=vec2((p.x+1.)*.5,flip==1?(p.y+1.)*.5:(1.-p.y)*.5); gl_Position=vec4(p*transform.xy+transform.zw,0,1); })";
    const char* fs=R"(#version 300 es
      precision highp float; uniform sampler2D image; uniform int mode; uniform float headroom; uniform float peak; in vec2 uv; out vec4 color;
      void main(){ vec3 c=texture(image,uv).rgb;
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
    glGenTextures(1,&p.texture); glBindTexture(GL_TEXTURE_2D,p.texture); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE); glGenVertexArrays(1,&p.vao);
    if(mode==1 && !p.makeSlots(64,64)) return false;
    p.mode=mode; return true;
  };
  for(int mode: {1,2,3}) { if(tryMode(mode)) { error.clear(); return mode; } detach(); }
  error="No supported HDR SurfaceControl, PQ, or linear floating-point window configuration"; return 0;
}
FrameResult Renderer::render(int w,int h,float scale,float x,float y,float displayHeadroom) {
  auto& p=*impl_; if(p.surface==EGL_NO_SURFACE || !p.image.width || w<=0 || h<=0 || !std::isfinite(scale) || scale<=0 || !std::isfinite(x) || !std::isfinite(y)) return FrameResult::Busy;
  if(!eglMakeCurrent(p.display,p.surface,p.surface,p.context)) return FrameResult::Busy;
  glBindTexture(GL_TEXTURE_2D,p.texture);
  if(!p.uploaded) { GLint max=0; glGetIntegerv(GL_MAX_TEXTURE_SIZE,&max); if(p.image.width>unsigned(max)||p.image.height>unsigned(max)) return FrameResult::Unavailable; glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA16F,p.image.width,p.image.height,0,GL_RGBA,GL_HALF_FLOAT,p.image.rgba_fp16.data()); if(glGetError()!=GL_NO_ERROR) return FrameResult::Unavailable; p.uploaded=true; }
  int index=-1; auto& api=surfaceApi();
  if(p.mode==1) {
    if(!p.makeSlots(w,h)) return FrameResult::Busy;
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
  float drawWidth=p.image.width*scale, drawHeight=p.image.height*scale;
  glViewport(0,0,w,h); glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT); glUseProgram(p.program); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,p.texture);
  float yOffset=1-(2*y+drawHeight)/h;
  glUniform4f(glGetUniformLocation(p.program,"transform"),drawWidth/w,drawHeight/h,(2*x+drawWidth)/w-1,p.mode==1 ? -yOffset : yOffset);
  glUniform1i(glGetUniformLocation(p.program,"image"),0); glUniform1i(glGetUniformLocation(p.program,"mode"),p.mode); glUniform1i(glGetUniformLocation(p.program,"flip"),p.mode==1);
  float desired=std::clamp(p.peak,1.f,16.f);
  float headroom=(displayHeadroom>0.f && std::isfinite(displayHeadroom)) ? std::clamp(displayHeadroom,1.f,desired) : desired;
  glUniform1f(glGetUniformLocation(p.program,"headroom"),headroom); glUniform1f(glGetUniformLocation(p.program,"peak"),desired);
  glBindVertexArray(p.vao); glDrawArrays(GL_TRIANGLE_STRIP,0,4);
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
    if (current) {
      if (p.texture) glDeleteTextures(1, &p.texture);
      if (p.program) glDeleteProgram(p.program);
      if (p.vao) glDeleteVertexArrays(1, &p.vao);
    }
    eglMakeCurrent(p.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (p.surface != EGL_NO_SURFACE) eglDestroySurface(p.display, p.surface);
    if (p.context != EGL_NO_CONTEXT) eglDestroyContext(p.display, p.context);
  }
  p.display=EGL_NO_DISPLAY; p.surface=EGL_NO_SURFACE; p.context=EGL_NO_CONTEXT; p.texture=p.program=p.vao=0; p.mode=0; p.uploaded=false;
}
}
