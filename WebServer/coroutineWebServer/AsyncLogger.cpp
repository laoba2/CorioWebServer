#include "AsyncLogger.h"

#include <chrono>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace cws {

AsyncLogger::LogQueue::LogQueue(std::size_t capacity)
    : slots_(capacity), mask_(capacity - 1) {
  if (capacity == 0 || (capacity & (capacity - 1)) != 0) {
    throw std::invalid_argument("AsyncLogger queue capacity must be a power of two");
  }
  for (std::size_t i = 0; i < slots_.size(); ++i) {
    slots_[i].sequence.store(i, std::memory_order_relaxed);
  }
}

bool AsyncLogger::LogQueue::push(std::shared_ptr<std::string> entry) {
  std::size_t pos = tail_.load(std::memory_order_relaxed);
  for (;;) {
    Slot& slot = slots_[pos & mask_];
    const std::size_t seq = slot.sequence.load(std::memory_order_acquire);
    const auto diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);
    if (diff == 0) {
      if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
        slot.payload.store(std::move(entry), std::memory_order_release);
        slot.sequence.store(pos + 1, std::memory_order_release);
        return true;
      }
    } else if (diff < 0) {
      return false;
    } else {
      pos = tail_.load(std::memory_order_relaxed);
    }
  }
}

bool AsyncLogger::LogQueue::pop(std::shared_ptr<std::string>& entry) {
  std::size_t pos = head_.load(std::memory_order_relaxed);
  for (;;) {
    Slot& slot = slots_[pos & mask_];
    const std::size_t seq = slot.sequence.load(std::memory_order_acquire);
    const auto diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1);
    if (diff == 0) {
      if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
        entry = slot.payload.exchange(nullptr, std::memory_order_acq_rel);
        slot.sequence.store(pos + mask_ + 1, std::memory_order_release);
        return static_cast<bool>(entry);
      }
    } else if (diff < 0) {
      return false;
    } else {
      pos = head_.load(std::memory_order_relaxed);
    }
  }
}

AsyncLogger::AsyncLogger(std::string file_path, std::size_t queue_capacity)
    : file_path_(std::move(file_path)), queue_capacity_(queue_capacity), queue_(queue_capacity_) {}

AsyncLogger::~AsyncLogger() {
  stop();
}

void AsyncLogger::start() {
  if (running_.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(sink_mutex_);
    sink_.open(file_path_, std::ios::out | std::ios::app);
    if (!sink_) {
      running_.store(false, std::memory_order_release);
      throw std::runtime_error("failed to open log file: " + file_path_);
    }
  }
  worker_ = std::thread(&AsyncLogger::worker_loop, this);
}

void AsyncLogger::stop() {
  if (!running_.exchange(false, std::memory_order_acq_rel)) {
    return;
  }
  if (worker_.joinable()) {
    worker_.join();
  }
  std::lock_guard<std::mutex> lock(sink_mutex_);
  if (sink_.is_open()) {
    sink_.flush();
    sink_.close();
  }
}

void AsyncLogger::log(LogLevel level, std::string message) {
  auto line = std::make_shared<std::string>(format_line(level, message));
  for (int attempts = 0; attempts < 16; ++attempts) {
    if (queue_.push(line)) {
      return;
    }
    std::this_thread::yield();
  }
  direct_write(*line);
}

void AsyncLogger::log(LogLevel level, std::string_view message) {
  log(level, std::string(message));
}

std::string AsyncLogger::level_name(LogLevel level) {
  switch (level) {
    case LogLevel::trace:
      return "TRACE";
    case LogLevel::debug:
      return "DEBUG";
    case LogLevel::info:
      return "INFO";
    case LogLevel::warn:
      return "WARN";
    case LogLevel::error:
      return "ERROR";
    case LogLevel::fatal:
      return "FATAL";
  }
  return "INFO";
}

std::string AsyncLogger::format_timestamp() {
  const auto now = std::chrono::system_clock::now();
  const auto tt = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &tt);
#else
  localtime_r(&tt, &tm);
#endif
  std::ostringstream oss;
  oss << std::put_time(&tm, "%F %T");
  return oss.str();
}

std::string AsyncLogger::format_line(LogLevel level, std::string_view message) const {
  std::ostringstream oss;
  oss << format_timestamp() << " [" << level_name(level) << "] " << message << '\n';
  return oss.str();
}

void AsyncLogger::direct_write(const std::string& line) {
  std::lock_guard<std::mutex> lock(sink_mutex_);
  if (sink_.is_open()) {
    sink_ << line;
    sink_.flush();
  }
}

void AsyncLogger::worker_loop() {
  while (running_.load(std::memory_order_acquire)) {
    std::shared_ptr<std::string> entry;
    bool wrote = false;
    while (queue_.pop(entry)) {
      if (entry) {
        std::lock_guard<std::mutex> lock(sink_mutex_);
        if (sink_.is_open()) {
          sink_ << *entry;
          wrote = true;
        }
      }
    }
    if (wrote) {
      std::lock_guard<std::mutex> lock(sink_mutex_);
      if (sink_.is_open()) {
        sink_.flush();
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  std::shared_ptr<std::string> entry;
  while (queue_.pop(entry)) {
    if (entry) {
      std::lock_guard<std::mutex> lock(sink_mutex_);
      if (sink_.is_open()) {
        sink_ << *entry;
      }
    }
  }
  std::lock_guard<std::mutex> lock(sink_mutex_);
  if (sink_.is_open()) {
    sink_.flush();
  }
}

}  // namespace cws
