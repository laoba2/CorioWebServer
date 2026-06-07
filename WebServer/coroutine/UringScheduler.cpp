#include "UringScheduler.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace cws {

thread_local UringScheduler* UringScheduler::current_ = nullptr;

void OperationBase::await_suspend(std::coroutine_handle<> handle) {
  continuation = handle;
  scheduler = UringScheduler::current();
  if (!scheduler) {
    throw std::runtime_error("Operation awaited outside UringScheduler context");
  }
  submit(*scheduler);
}

void OperationBase::complete(int res, unsigned cqe_flags) noexcept {
  if (completed.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  result = res;
  flags = cqe_flags;
  if (scheduler && continuation) {
    scheduler->enqueue(continuation, false);
  }
}

int OperationBase::await_resume() {
  if (result < 0) {
    throw std::system_error(-result, std::generic_category());
  }
  return result;
}

UringScheduler::UringScheduler(unsigned entries) : entries_(entries) {
  if (entries_ == 0) {
    entries_ = 1024;
  }
  const int rc = io_uring_queue_init(entries_, &ring_, 0);
  if (rc < 0) {
    throw std::system_error(-rc, std::generic_category(), "io_uring_queue_init failed");
  }
}

UringScheduler::~UringScheduler() {
  stop();
  io_uring_queue_exit(&ring_);
}

void UringScheduler::stop() noexcept {
  running_.store(false, std::memory_order_release);
}

void UringScheduler::submit() {
  const int rc = io_uring_submit(&ring_);
  if (rc < 0) {
    throw std::system_error(-rc, std::generic_category(), "io_uring_submit failed");
  }
}

void UringScheduler::enqueue(std::coroutine_handle<> handle, bool destroy_after_resume) {
  if (!handle) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(ready_mutex_);
    ready_.push_back({handle, destroy_after_resume});
  }
}

void UringScheduler::post(std::function<void()> fn) {
  {
    std::lock_guard<std::mutex> lock(post_mutex_);
    posts_.push_back(std::move(fn));
  }
}

void UringScheduler::resume_ready(ReadyItem item) {
  if (!item.handle) {
    return;
  }
  item.handle.resume();
  if (item.destroy_after_resume && item.handle.done()) {
    item.handle.destroy();
  }
}

void UringScheduler::drain_posts() {
  std::deque<std::function<void()>> local;
  {
    std::lock_guard<std::mutex> lock(post_mutex_);
    local.swap(posts_);
  }
  for (auto& fn : local) {
    fn();
  }
}

void UringScheduler::drain_ready() {
  std::deque<ReadyItem> local;
  {
    std::lock_guard<std::mutex> lock(ready_mutex_);
    local.swap(ready_);
  }
  for (auto& item : local) {
    resume_ready(item);
  }
}

void UringScheduler::handle_cqe(io_uring_cqe* cqe) {
  auto* op = reinterpret_cast<OperationBase*>(io_uring_cqe_get_data(cqe));
  if (!op) {
    return;
  }
  op->complete(cqe->res, cqe->flags);
}

void UringScheduler::run() {
  current_ = this;
  running_.store(true, std::memory_order_release);

  while (running_.load(std::memory_order_acquire)) {
    drain_posts();
    drain_ready();

    io_uring_submit(&ring_);

    io_uring_cqe* cqe = nullptr;
    int rc = io_uring_peek_cqe(&ring_, &cqe);
    if (rc == 0 && cqe != nullptr) {
      while (cqe != nullptr) {
        handle_cqe(cqe);
        io_uring_cqe_seen(&ring_, cqe);
        cqe = nullptr;
        rc = io_uring_peek_cqe(&ring_, &cqe);
        if (rc < 0) {
          if (rc == -EINTR) {
            continue;
          }
          break;
        }
      }
      continue;
    }

    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }

  drain_posts();
  drain_ready();
  current_ = nullptr;
}

int UringScheduler::submit_or_throw(OperationBase& op) {
  op.submit(*this);
  submit();
  return 0;
}

void UringScheduler::AcceptAwaiter::submit(UringScheduler& scheduler) {
  io_uring_sqe* sqe = io_uring_get_sqe(&scheduler.ring_);
  if (!sqe) {
    scheduler.submit();
    sqe = io_uring_get_sqe(&scheduler.ring_);
  }
  if (!sqe) {
    throw std::runtime_error("io_uring queue is full");
  }
#if defined(IO_URING_OP_MULTISHOT_ACCEPT) || defined(IORING_OP_MULTISHOT_ACCEPT)
  if (multishot_) {
    io_uring_prep_multishot_accept(sqe, fd_, addr_, addrlen_, flags_);
  } else {
    io_uring_prep_accept(sqe, fd_, addr_, addrlen_, flags_);
  }
#else
  (void)multishot_;
  io_uring_prep_accept(sqe, fd_, addr_, addrlen_, flags_);
#endif
  io_uring_sqe_set_data(sqe, this);
}

void UringScheduler::RecvAwaiter::submit(UringScheduler& scheduler) {
  io_uring_sqe* recv_sqe = io_uring_get_sqe(&scheduler.ring_);
  if (!recv_sqe) {
    scheduler.submit();
    recv_sqe = io_uring_get_sqe(&scheduler.ring_);
  }
  if (!recv_sqe) {
    throw std::runtime_error("io_uring queue is full");
  }
  io_uring_prep_recv(recv_sqe, fd_, buffer_, static_cast<unsigned>(length_), flags_);
  io_uring_sqe_set_data(recv_sqe, this);

  if (timeout_.has_value()) {
    io_uring_sqe* timeout_sqe = io_uring_get_sqe(&scheduler.ring_);
    if (!timeout_sqe) {
      scheduler.submit();
      timeout_sqe = io_uring_get_sqe(&scheduler.ring_);
    }
    if (!timeout_sqe) {
      throw std::runtime_error("io_uring queue is full");
    }
    const auto sec = std::chrono::duration_cast<std::chrono::seconds>(*timeout_);
    const auto nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(*timeout_ - sec);
    __kernel_timespec ts{};
    ts.tv_sec = sec.count();
    ts.tv_nsec = nsec.count();
    recv_sqe->flags |= IOSQE_IO_LINK;
    io_uring_prep_timeout(timeout_sqe, &ts, 0, 0);
    io_uring_sqe_set_data(timeout_sqe, this);
  }
}

void UringScheduler::SendAwaiter::submit(UringScheduler& scheduler) {
  io_uring_sqe* sqe = io_uring_get_sqe(&scheduler.ring_);
  if (!sqe) {
    scheduler.submit();
    sqe = io_uring_get_sqe(&scheduler.ring_);
  }
  if (!sqe) {
    throw std::runtime_error("io_uring queue is full");
  }
  io_uring_prep_send(sqe, fd_, buffer_, static_cast<unsigned>(length_), flags_);
  io_uring_sqe_set_data(sqe, this);
}

void UringScheduler::ReadAwaiter::submit(UringScheduler& scheduler) {
  io_uring_sqe* sqe = io_uring_get_sqe(&scheduler.ring_);
  if (!sqe) {
    scheduler.submit();
    sqe = io_uring_get_sqe(&scheduler.ring_);
  }
  if (!sqe) {
    throw std::runtime_error("io_uring queue is full");
  }
  if (offset_ >= 0) {
    io_uring_prep_read(sqe, fd_, buffer_, static_cast<unsigned>(length_), offset_);
  } else {
    io_uring_prep_read(sqe, fd_, buffer_, static_cast<unsigned>(length_), 0);
  }
  io_uring_sqe_set_data(sqe, this);
}

void UringScheduler::WriteAwaiter::submit(UringScheduler& scheduler) {
  io_uring_sqe* sqe = io_uring_get_sqe(&scheduler.ring_);
  if (!sqe) {
    scheduler.submit();
    sqe = io_uring_get_sqe(&scheduler.ring_);
  }
  if (!sqe) {
    throw std::runtime_error("io_uring queue is full");
  }
  if (offset_ >= 0) {
    io_uring_prep_write(sqe, fd_, buffer_, static_cast<unsigned>(length_), offset_);
  } else {
    io_uring_prep_write(sqe, fd_, buffer_, static_cast<unsigned>(length_), 0);
  }
  io_uring_sqe_set_data(sqe, this);
}

void UringScheduler::TimeoutAwaiter::submit(UringScheduler& scheduler) {
  io_uring_sqe* sqe = io_uring_get_sqe(&scheduler.ring_);
  if (!sqe) {
    scheduler.submit();
    sqe = io_uring_get_sqe(&scheduler.ring_);
  }
  if (!sqe) {
    throw std::runtime_error("io_uring queue is full");
  }
  const auto sec = std::chrono::duration_cast<std::chrono::seconds>(timeout_);
  const auto nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(timeout_ - sec);
  __kernel_timespec ts{};
  ts.tv_sec = sec.count();
  ts.tv_nsec = nsec.count();
  io_uring_prep_timeout(sqe, &ts, 0, 0);
  io_uring_sqe_set_data(sqe, this);
}

void UringScheduler::CancelAwaiter::submit(UringScheduler& scheduler) {
  io_uring_sqe* sqe = io_uring_get_sqe(&scheduler.ring_);
  if (!sqe) {
    scheduler.submit();
    sqe = io_uring_get_sqe(&scheduler.ring_);
  }
  if (!sqe) {
    throw std::runtime_error("io_uring queue is full");
  }
  io_uring_prep_cancel(sqe, reinterpret_cast<void*>(token_), 0);
  io_uring_sqe_set_data(sqe, this);
}

auto UringScheduler::async_accept(int fd, sockaddr* addr, socklen_t* addrlen, int flags, bool multishot) {
  return AcceptAwaiter{*this, fd, addr, addrlen, flags, multishot};
}

auto UringScheduler::async_recv(int fd, void* buffer, size_t length, int flags) {
  return RecvAwaiter{*this, fd, buffer, length, flags};
}

auto UringScheduler::async_recv_for(int fd, void* buffer, size_t length, Ns timeout, int flags) {
  return RecvAwaiter{*this, fd, buffer, length, flags, timeout};
}

auto UringScheduler::async_send(int fd, const void* buffer, size_t length, int flags) {
  return SendAwaiter{*this, fd, buffer, length, flags};
}

auto UringScheduler::async_read(int fd, void* buffer, size_t length, off_t offset) {
  return ReadAwaiter{*this, fd, buffer, length, offset};
}

auto UringScheduler::async_write(int fd, const void* buffer, size_t length, off_t offset) {
  return WriteAwaiter{*this, fd, buffer, length, offset};
}

auto UringScheduler::async_timeout(Ns timeout) {
  return TimeoutAwaiter{*this, timeout};
}

auto UringScheduler::async_cancel(uint64_t token) {
  return CancelAwaiter{*this, token};
}

}  // namespace cws
