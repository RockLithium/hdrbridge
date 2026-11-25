#pragma once

#include "hdrbridge_threads.h"
#include <algorithm>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace hdrbridge::detail {

template <typename Function>
void parallel_pixel_blocks(size_t count, Function&& function) {
  constexpr size_t kMinimumPixelsPerWorker = 262144;
  const unsigned hardware = default_worker_count();
  const size_t useful_workers = std::max<size_t>(
      1, (count + kMinimumPixelsPerWorker - 1) / kMinimumPixelsPerWorker);
  const size_t workers = std::min<size_t>(hardware, useful_workers);
  if (workers <= 1 || count < kMinimumPixelsPerWorker * 2) {
    function(0, count);
    return;
  }
  std::vector<std::thread> threads;
  threads.reserve(workers);
  std::mutex error_mutex;
  std::exception_ptr error;
  const auto run = [&](size_t worker) {
      const size_t begin = count * worker / workers;
      const size_t end = count * (worker + 1) / workers;
      try {
        function(begin, end);
      } catch (...) {
        std::lock_guard lock(error_mutex);
        if (!error) error = std::current_exception();
      }
  };
  try {
    for (size_t worker = 1; worker < workers; ++worker) {
      threads.emplace_back(run, worker);
    }
  } catch (...) {
    for (auto& thread : threads) thread.join();
    throw;
  }
  run(0);
  for (auto& thread : threads) thread.join();
  if (error) std::rethrow_exception(error);
}

template <typename Function>
size_t parallel_indexed_blocks(size_t count, Function&& function) {
  constexpr size_t kMinimumPixelsPerWorker = 262144;
  const unsigned hardware = default_worker_count();
  const size_t useful_workers = std::max<size_t>(
      1, (count + kMinimumPixelsPerWorker - 1) / kMinimumPixelsPerWorker);
  const size_t workers = std::min<size_t>(hardware, useful_workers);
  if (workers <= 1 || count < kMinimumPixelsPerWorker * 2) {
    function(0, 0, count);
    return 1;
  }
  std::vector<std::thread> threads;
  threads.reserve(workers);
  std::mutex error_mutex;
  std::exception_ptr error;
  const auto run = [&](size_t worker) {
      const size_t begin = count * worker / workers;
      const size_t end = count * (worker + 1) / workers;
      try {
        function(worker, begin, end);
      } catch (...) {
        std::lock_guard lock(error_mutex);
        if (!error) error = std::current_exception();
      }
  };
  try {
    for (size_t worker = 1; worker < workers; ++worker) {
      threads.emplace_back(run, worker);
    }
  } catch (...) {
    for (auto& thread : threads) thread.join();
    throw;
  }
  run(0);
  for (auto& thread : threads) thread.join();
  if (error) std::rethrow_exception(error);
  return workers;
}

}  // namespace hdrbridge::detail
