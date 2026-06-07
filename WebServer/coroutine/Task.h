#pragma once

#include "MemoryResource.h"

#include <coroutine>
#include <exception>
#include <optional>
#include <type_traits>
#include <utility>

namespace cws {

class UringScheduler;
void scheduler_enqueue(UringScheduler& scheduler, std::coroutine_handle<> handle, bool destroy_after_resume);

template <typename T>
class Task {
 public:
  struct promise_type {
    std::optional<T> value_;
    std::exception_ptr exception_;
    std::coroutine_handle<> continuation_{};

    static void* operator new(std::size_t size) { return allocate_coroutine_frame(size); }
    static void operator delete(void* ptr, std::size_t size) noexcept { deallocate_coroutine_frame(ptr, size); }

    Task get_return_object() noexcept { return Task{handle_type::from_promise(*this)}; }
    std::suspend_always initial_suspend() noexcept { return {}; }

    struct FinalAwaiter {
      bool await_ready() noexcept { return false; }

      template <typename Promise>
      std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> handle) noexcept {
        auto& promise = handle.promise();
        if (promise.continuation_) {
          return promise.continuation_;
        }
        return std::noop_coroutine();
      }

      void await_resume() noexcept {}
    };

    FinalAwaiter final_suspend() noexcept { return {}; }

    template <typename U>
    void return_value(U&& value) noexcept(std::is_nothrow_constructible_v<T, U&&>) {
      value_.emplace(std::forward<U>(value));
    }

    void unhandled_exception() noexcept { exception_ = std::current_exception(); }
  };

  using handle_type = std::coroutine_handle<promise_type>;

  Task() = default;
  explicit Task(handle_type handle) noexcept : handle_(handle) {}
  Task(const Task&) = delete;
  Task& operator=(const Task&) = delete;

  Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
  Task& operator=(Task&& other) noexcept {
    if (this != &other) {
      if (handle_) {
        handle_.destroy();
      }
      handle_ = std::exchange(other.handle_, {});
    }
    return *this;
  }

  ~Task() {
    if (handle_) {
      handle_.destroy();
    }
  }

  bool valid() const noexcept { return static_cast<bool>(handle_); }

  void start(UringScheduler& scheduler) && {
    if (handle_) {
      scheduler_enqueue(scheduler, handle_, true);
      handle_ = {};
    }
  }

  struct Awaiter {
    handle_type handle_;

    bool await_ready() const noexcept { return !handle_ || handle_.done(); }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting) noexcept {
      handle_.promise().continuation_ = awaiting;
      return handle_;
    }

    T await_resume() {
      auto& promise = handle_.promise();
      if (promise.exception_) {
        handle_.destroy();
        std::rethrow_exception(promise.exception_);
      }
      T value = std::move(*promise.value_);
      handle_.destroy();
      return value;
    }
  };

  auto operator co_await() && noexcept { return Awaiter{std::exchange(handle_, {})}; }

 private:
  handle_type handle_{};
};

template <>
class Task<void> {
 public:
  struct promise_type {
    std::exception_ptr exception_;
    std::coroutine_handle<> continuation_{};

    static void* operator new(std::size_t size) { return allocate_coroutine_frame(size); }
    static void operator delete(void* ptr, std::size_t size) noexcept { deallocate_coroutine_frame(ptr, size); }

    Task get_return_object() noexcept { return Task{handle_type::from_promise(*this)}; }
    std::suspend_always initial_suspend() noexcept { return {}; }

    struct FinalAwaiter {
      bool await_ready() noexcept { return false; }

      template <typename Promise>
      std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> handle) noexcept {
        auto& promise = handle.promise();
        if (promise.continuation_) {
          return promise.continuation_;
        }
        return std::noop_coroutine();
      }

      void await_resume() noexcept {}
    };

    FinalAwaiter final_suspend() noexcept { return {}; }
    void return_void() noexcept {}
    void unhandled_exception() noexcept { exception_ = std::current_exception(); }
  };

  using handle_type = std::coroutine_handle<promise_type>;

  Task() = default;
  explicit Task(handle_type handle) noexcept : handle_(handle) {}
  Task(const Task&) = delete;
  Task& operator=(const Task&) = delete;

  Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
  Task& operator=(Task&& other) noexcept {
    if (this != &other) {
      if (handle_) {
        handle_.destroy();
      }
      handle_ = std::exchange(other.handle_, {});
    }
    return *this;
  }

  ~Task() {
    if (handle_) {
      handle_.destroy();
    }
  }

  bool valid() const noexcept { return static_cast<bool>(handle_); }

  void start(UringScheduler& scheduler) && {
    if (handle_) {
      scheduler_enqueue(scheduler, handle_, true);
      handle_ = {};
    }
  }

  struct Awaiter {
    handle_type handle_;

    bool await_ready() const noexcept { return !handle_ || handle_.done(); }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting) noexcept {
      handle_.promise().continuation_ = awaiting;
      return handle_;
    }

    void await_resume() {
      auto& promise = handle_.promise();
      if (promise.exception_) {
        handle_.destroy();
        std::rethrow_exception(promise.exception_);
      }
      handle_.destroy();
    }
  };

  auto operator co_await() && noexcept { return Awaiter{std::exchange(handle_, {})}; }

 private:
  handle_type handle_{};
};

}  // namespace cws
