#pragma once

#include "Task.h"
#include "Types.h"

#include <atomic>
#include <coroutine>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <system_error>
#include <thread>
#include <vector>

#include <liburing.h>

namespace cws {

class UringScheduler;
void scheduler_enqueue(UringScheduler& scheduler, std::coroutine_handle<> handle, bool destroy_after_resume);

struct OperationBase {
  UringScheduler* scheduler{nullptr};
  std::coroutine_handle<> continuation{};
  std::atomic<bool> completed{false};
  int result{0};
  unsigned flags{0};
  uint64_t token{0};

  virtual ~OperationBase() = default;
  virtual void submit(UringScheduler& scheduler) = 0;

  void await_suspend(std::coroutine_handle<> handle);
  bool await_ready() const noexcept { return false; }
  void complete(int res, unsigned cqe_flags) noexcept;
  int await_resume();
};

class UringScheduler {
 public:
  struct ReadyItem {
    std::coroutine_handle<> handle{};
    bool destroy_after_resume{false};
  };

  explicit UringScheduler(unsigned entries = 1024);
  ~UringScheduler();

  UringScheduler(const UringScheduler&) = delete;
  UringScheduler& operator=(const UringScheduler&) = delete;

  void run();
  void stop() noexcept;
  bool running() const noexcept { return running_.load(std::memory_order_acquire); }
  void submit();

  void enqueue(std::coroutine_handle<> handle, bool destroy_after_resume = false);
  void post(std::function<void()> fn);

  static UringScheduler* current() noexcept { return current_; }

  int submit_or_throw(OperationBase& op);

  class AcceptAwaiter;
  class RecvAwaiter;
  class SendAwaiter;
  class ReadAwaiter;
  class WriteAwaiter;
  class TimeoutAwaiter;
  class CancelAwaiter;

  auto async_accept(int fd, sockaddr* addr, socklen_t* addrlen, int flags = 0, bool multishot = false);
  auto async_recv(int fd, void* buffer, size_t length, int flags = 0);
  auto async_recv_for(int fd, void* buffer, size_t length, Ns timeout, int flags = 0);
  auto async_send(int fd, const void* buffer, size_t length, int flags = 0);
  auto async_read(int fd, void* buffer, size_t length, off_t offset = -1);
  auto async_write(int fd, const void* buffer, size_t length, off_t offset = -1);
  auto async_timeout(Ns timeout);
  auto async_cancel(uint64_t token);

 private:
  friend struct OperationBase;

  void resume_ready(ReadyItem item);
  void drain_ready();
  void drain_posts();
  void handle_cqe(io_uring_cqe* cqe);

  static thread_local UringScheduler* current_;

  io_uring ring_{};
  unsigned entries_{0};
  std::atomic<bool> running_{false};
  std::mutex ready_mutex_;
  std::deque<ReadyItem> ready_;
  std::mutex post_mutex_;
  std::deque<std::function<void()>> posts_;
};

class UringScheduler::AcceptAwaiter final : public OperationBase {
 public:
  AcceptAwaiter(UringScheduler& scheduler, int fd, sockaddr* addr, socklen_t* addrlen, int flags, bool multishot)
      : fd_(fd), addr_(addr), addrlen_(addrlen), flags_(flags), multishot_(multishot) {
    this->scheduler = &scheduler;
  }

  void submit(UringScheduler& scheduler) override;

 private:
  int fd_{-1};
  sockaddr* addr_{nullptr};
  socklen_t* addrlen_{nullptr};
  int flags_{0};
  bool multishot_{false};
};

class UringScheduler::RecvAwaiter final : public OperationBase {
 public:
  RecvAwaiter(UringScheduler& scheduler, int fd, void* buffer, size_t length, int flags, std::optional<Ns> timeout = std::nullopt)
      : fd_(fd), buffer_(buffer), length_(length), flags_(flags), timeout_(timeout) {
    this->scheduler = &scheduler;
  }

  void submit(UringScheduler& scheduler) override;

 private:
  int fd_{-1};
  void* buffer_{nullptr};
  size_t length_{0};
  int flags_{0};
  std::optional<Ns> timeout_{};
};

class UringScheduler::SendAwaiter final : public OperationBase {
 public:
  SendAwaiter(UringScheduler& scheduler, int fd, const void* buffer, size_t length, int flags)
      : fd_(fd), buffer_(buffer), length_(length), flags_(flags) {
    this->scheduler = &scheduler;
  }

  void submit(UringScheduler& scheduler) override;

 private:
  int fd_{-1};
  const void* buffer_{nullptr};
  size_t length_{0};
  int flags_{0};
};

class UringScheduler::ReadAwaiter final : public OperationBase {
 public:
  ReadAwaiter(UringScheduler& scheduler, int fd, void* buffer, size_t length, off_t offset)
      : fd_(fd), buffer_(buffer), length_(length), offset_(offset) {
    this->scheduler = &scheduler;
  }

  void submit(UringScheduler& scheduler) override;

 private:
  int fd_{-1};
  void* buffer_{nullptr};
  size_t length_{0};
  off_t offset_{-1};
};

class UringScheduler::WriteAwaiter final : public OperationBase {
 public:
  WriteAwaiter(UringScheduler& scheduler, int fd, const void* buffer, size_t length, off_t offset)
      : fd_(fd), buffer_(buffer), length_(length), offset_(offset) {
    this->scheduler = &scheduler;
  }

  void submit(UringScheduler& scheduler) override;

 private:
  int fd_{-1};
  const void* buffer_{nullptr};
  size_t length_{0};
  off_t offset_{-1};
};

class UringScheduler::TimeoutAwaiter final : public OperationBase {
 public:
  TimeoutAwaiter(UringScheduler& scheduler, Ns timeout) : timeout_(timeout) { this->scheduler = &scheduler; }

  void submit(UringScheduler& scheduler) override;

 private:
  Ns timeout_{};
};

class UringScheduler::CancelAwaiter final : public OperationBase {
 public:
  CancelAwaiter(UringScheduler& scheduler, uint64_t token) : token_(token) { this->scheduler = &scheduler; }

  void submit(UringScheduler& scheduler) override;

 private:
  uint64_t token_{0};
};

inline void scheduler_enqueue(UringScheduler& scheduler, std::coroutine_handle<> handle, bool destroy_after_resume) {
  scheduler.enqueue(handle, destroy_after_resume);
}

}  // namespace cws
