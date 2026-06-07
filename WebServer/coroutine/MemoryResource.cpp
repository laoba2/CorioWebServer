#include "MemoryResource.h"

#include <new>

#if __has_include(<mimalloc.h>)
#include <mimalloc.h>
#define CWS_HAS_MIMALLOC 1
#else
#define CWS_HAS_MIMALLOC 0
#endif

namespace cws {

namespace {

class MimallocResource final : public std::pmr::memory_resource {
 protected:
  void* do_allocate(std::size_t bytes, std::size_t alignment) override {
#if CWS_HAS_MIMALLOC
    return mi_malloc_aligned(bytes, alignment);
#else
    (void)alignment;
    return ::operator new(bytes);
#endif
  }

  void do_deallocate(void* ptr, std::size_t bytes, std::size_t alignment) override {
#if CWS_HAS_MIMALLOC
    (void)bytes;
    (void)alignment;
    mi_free(ptr);
#else
    (void)bytes;
    (void)alignment;
    ::operator delete(ptr);
#endif
  }

  bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }
};

std::pmr::memory_resource* global_fallback_resource() {
  static MimallocResource mimalloc_resource;
  static auto* new_delete_resource = std::pmr::new_delete_resource();
#if CWS_HAS_MIMALLOC
  return &mimalloc_resource;
#else
  return new_delete_resource;
#endif
}

}  // namespace

ThreadMemoryContext& ThreadMemoryContext::instance() {
  thread_local ThreadMemoryContext context;
  return context;
}

ThreadMemoryContext::ThreadMemoryContext()
    : upstream_(global_fallback_resource()),
      pool_(std::pmr::pool_options{16, 1024}, upstream_) {}

std::pmr::memory_resource* ThreadMemoryContext::resource() noexcept {
  return &pool_;
}

std::pmr::memory_resource* ThreadMemoryContext::fallback_resource() noexcept {
  return upstream_;
}

void* ThreadMemoryContext::allocate(std::size_t bytes, std::size_t alignment) {
  return resource()->allocate(bytes, alignment);
}

void ThreadMemoryContext::deallocate(void* ptr, std::size_t bytes, std::size_t alignment) noexcept {
  resource()->deallocate(ptr, bytes, alignment);
}

void* allocate_coroutine_frame(std::size_t bytes) {
  return ThreadMemoryContext::instance().allocate(bytes);
}

void deallocate_coroutine_frame(void* ptr, std::size_t bytes) noexcept {
  ThreadMemoryContext::instance().deallocate(ptr, bytes);
}

}  // namespace cws
