#pragma once
#include <cstdint>
#include <memory>
#include <string>
struct ANativeWindow;
namespace hdrbridge::android_preview {
enum class FrameResult { Unavailable = -1, Busy = 0, Drawn = 1 };
class Renderer {
 public:
  Renderer();
  ~Renderer();
  static int maxTextureSize();
  bool decode(const std::string&, uint32_t, std::string&);
  uint32_t width() const;
  uint32_t height() const;
  float peakRatio() const;
  int attach(ANativeWindow*, std::string&);
  FrameResult render(int, int, float, float, float, float = 0.0f);
  void detach();
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
