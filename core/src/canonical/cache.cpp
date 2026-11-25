#include "canonical/cache.h"
#include "canonical/input.h"
#include "infrastructure/source_revision.h"
#include "infrastructure/progress.h"
#include "infrastructure/timing.h"
#include <mutex>

namespace hdrbridge {
namespace detail {
namespace {

struct CanonicalCacheEntry {
  std::filesystem::path path;
  uintmax_t file_size = 0;
  std::filesystem::file_time_type write_time{};
  SourceRevision revision;
  std::shared_ptr<const DecodedImage> image;
};

struct CanonicalCacheState {
  std::mutex cache_mutex;
  // Preview, Inspector and conversion can request the same file concurrently.
  // Serialize cache misses so only one full-resolution source is decoded.
  std::mutex decode_mutex;
  CanonicalCacheEntry cache;
  uint64_t generation = 0;
};

CanonicalCacheState& canonical_cache_state() {
  static auto* state = new CanonicalCacheState();
  return *state;
}

}  // namespace

std::shared_ptr<const DecodedImage> decode_input(
    const std::filesystem::path& path,
    const ProgressCallback& progress,
    std::atomic_bool* cancel,
    TimingDiagnostics& timings) {
  auto& cache_state = canonical_cache_state();
  check_cancel(cancel);
  std::lock_guard decode_lock(cache_state.decode_mutex);
  check_cancel(cancel);
  const auto absolute = std::filesystem::weakly_canonical(path);
  const uintmax_t size = std::filesystem::file_size(absolute);
  const auto write_time = std::filesystem::last_write_time(absolute);
  const auto revision = source_revision(absolute);
  uint64_t cache_generation;
  std::shared_ptr<const DecodedImage> cached;
  {
    std::lock_guard lock(cache_state.cache_mutex);
    check_cancel(cancel);
    cache_generation = cache_state.generation;
    if (cache_state.cache.image && cache_state.cache.path == absolute &&
        cache_state.cache.file_size == size && cache_state.cache.write_time == write_time &&
        revision.valid && cache_state.cache.revision == revision) {
      cached = cache_state.cache.image;
    } else {
      cache_state.cache = {};
      cache_state.cache.path = absolute;
    }
  }
  if (cached) {
    timings = {};
    timings.canonical_cache_hit = true;
    report(progress, 42, "Reusing cached canonical high-precision HDR master");
    return cached;
  }
  auto decoded = std::make_shared<DecodedImage>(
      decode_input_uncached(absolute, progress, cancel));
  timings = decoded->timings;
  timings.canonical_cache_hit = false;
  if (std::filesystem::file_size(absolute) != size ||
      std::filesystem::last_write_time(absolute) != write_time ||
      (revision.valid && source_revision(absolute) != revision)) {
    throw std::runtime_error("Source changed while decoding; reopen the image");
  }
  if (revision.valid && (decoded->info.asset_kind == "direct-hdr" ||
      decoded->info.asset_kind == "gain-map-hdr")) {
    std::lock_guard lock(cache_state.cache_mutex);
    if (cache_generation == cache_state.generation) {
      cache_state.cache = {absolute, size, write_time, revision, decoded};
    }
  }
  return decoded;
}

}  // namespace detail

using namespace detail;

void clear_input_cache() {
  auto& cache_state = canonical_cache_state();
  std::lock_guard lock(cache_state.cache_mutex);
  ++cache_state.generation;
  cache_state.cache = {};
}

void discard_cached_input(const std::filesystem::path& path) {
  std::error_code error;
  const auto absolute = std::filesystem::weakly_canonical(path, error);
  if (error) return;
  auto& cache_state = canonical_cache_state();
  std::lock_guard lock(cache_state.cache_mutex);
  if (cache_state.cache.path == absolute) {
    ++cache_state.generation;
    cache_state.cache = {};
  }
}

}  // namespace hdrbridge
