#pragma once

#include <algorithm>
#include <thread>

namespace hdrbridge {

// Scalar WASM cannot create pthreads. Jobs with few pixels may use fewer
// workers than the native/threaded-WASM default budget.
inline unsigned default_worker_count() {
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
  return 1u;
#else
  const unsigned detected = std::thread::hardware_concurrency();
  return detected ? detected : 4u;
#endif
}

// libheif/JXL count background workers; zero runs on the calling thread.
inline unsigned codec_background_workers() {
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
  return 0u;
#else
  return default_worker_count();
#endif
}

}  // namespace hdrbridge
