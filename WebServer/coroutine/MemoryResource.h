#pragma once

#include "Types.h"

#include <memory_resource>

namespace cws {

class ThreadMemoryContext {
 public:
  static ThreadMemoryContext& instance();

  std::pmr::memory_resource* resource() noexcept;
  std::pmr::memory_resource* fallback_resource() noexcept;

  void* allocate(std::size_t bytes, std::size_t alignment = alignof(std::max_align_t));
  void deallocate(void* ptr, std::size_t bytes, std::size_t alignment = alignof(std::max_align_t)) noexcept;

 private:
  ThreadMemoryContext();
  std::pmr::memory_resource* upstream_;
  std::pmr::unsynchronized_pool_resource pool_;
};

void* allocate_coroutine_frame(std::size_t bytes);
void deallocate_coroutine_frame(void* ptr, std::size_t bytes) noexcept;

}  // namespace cws
