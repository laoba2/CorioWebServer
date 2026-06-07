#pragma once

#include <atomic>
#include <cstddef>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <utility>

#if __has_include(<format>)
#include <format>
#define CWS_HAS_FORMAT 1
#else
#define CWS_HAS_FORMAT 0
#endif

namespace cws {

enum class LogLevel { trace = 0, debug, info, warn, error, fatal };

class AsyncLogger {
 public:
  explicit AsyncLogger(std::string file_path, std::size_t queue_capacity = 8192);
  ~AsyncLogger();

  AsyncLogger(const AsyncLogger&) = delete;
  AsyncLogger& operator=(const AsyncLogger&) = delete;

  void start();
  void stop();

  void log(LogLevel level, std::string message);
  void log(LogLevel level, std::string_view message);

  template <typename... Args>
  void logf(LogLevel level, std::string_view fmt, Args&&... args) {
#if CWS_HAS_FORMAT
    log(level, std::format(fmt, std::forward<Args>(args)...));
#else
    (void)fmt;
    log(level, std::string("format support unavailable"));
    (void)sizeof...(args);
#endif
  }

  template <typename... Args>
  void trace(std::string_view fmt, Args&&... args) {
    logf(LogLevel::trace, fmt, std::forward<Args>(args)...);
  }

  template <typename... Args>
  void debug(std::string_view fmt, Args&&... args) {
    logf(LogLevel::debug, fmt, std::forward<Args>(args)...);
  }

  template <typename... Args>
  void info(std::string_view fmt, Args&&... args) {
    logf(LogLevel::info, fmt, std::forward<Args>(args)...);
  }

  template <typename... Args>
  void warn(std::string_view fmt, Args&&... args) {
    logf(LogLevel::warn, fmt, std::forward<Args>(args)...);
  }

  template <typename... Args>
  void error(std::string_view fmt, Args&&... args) {
    logf(LogLevel::error, fmt, std::forward<Args>(args)...);
  }

 private:
  struct LogQueue {
    struct Slot {
      std::atomic<std::size_t> sequence;
      std::atomic<std::shared_ptr<std::string>> payload;
      Slot() : sequence(0), payload(nullptr) {}
    };

    explicit LogQueue(std::size_t capacity);
    bool push(std::shared_ptr<std::string> entry);
    bool pop(std::shared_ptr<std::string>& entry);

    std::vector<Slot> slots_;
    const std::size_t mask_;
    std::atomic<std::size_t> head_{0};
    std::atomic<std::size_t> tail_{0};
  };

  static std::string level_name(LogLevel level);
  static std::string format_timestamp();
  std::string format_line(LogLevel level, std::string_view message) const;
  void worker_loop();
  void direct_write(const std::string& line);

  std::string file_path_;
  std::size_t queue_capacity_{0};
  std::atomic<bool> running_{false};
  std::thread worker_;
  LogQueue queue_;
  std::ofstream sink_;
  std::mutex sink_mutex_;
};

}  // namespace cws
