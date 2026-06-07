#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fcntl.h>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cws {

using Clock = std::chrono::steady_clock;
using Ns = std::chrono::nanoseconds;

class UniqueFd {
 public:
  UniqueFd() = default;
  explicit UniqueFd(int fd, bool direct = false) noexcept : fd_(fd), direct_(direct) {}

  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;

  UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)), direct_(other.direct_) {}
  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
      reset();
      fd_ = std::exchange(other.fd_, -1);
      direct_ = other.direct_;
    }
    return *this;
  }

  ~UniqueFd() { reset(); }

  int get() const noexcept { return fd_; }
  bool direct() const noexcept { return direct_; }
  explicit operator bool() const noexcept { return fd_ >= 0; }

  int release() noexcept {
    direct_ = false;
    return std::exchange(fd_, -1);
  }

  void reset(int fd = -1, bool direct = false) noexcept {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = fd;
    direct_ = direct;
  }

  UniqueFd duplicate() const {
    if (fd_ < 0) {
      return UniqueFd{};
    }
    return UniqueFd(::dup(fd_), direct_);
  }

  static UniqueFd open_regular(const std::filesystem::path& path, int flags, mode_t mode = 0644) {
    return UniqueFd(::open(path.c_str(), flags, mode), false);
  }

  static UniqueFd open_direct(const std::filesystem::path& path, int flags, mode_t mode = 0644) {
#ifdef O_DIRECT
    return UniqueFd(::open(path.c_str(), flags | O_DIRECT, mode), true);
#else
    return UniqueFd(::open(path.c_str(), flags, mode), true);
#endif
  }

 private:
  int fd_{-1};
  bool direct_{false};
};

inline std::string to_lower(std::string_view input) {
  std::string out(input);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

inline std::string trim(std::string_view input) {
  size_t begin = 0;
  while (begin < input.size() && std::isspace(static_cast<unsigned char>(input[begin]))) {
    ++begin;
  }
  size_t end = input.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1]))) {
    --end;
  }
  return std::string(input.substr(begin, end - begin));
}

struct HttpRequest {
  std::string method;
  std::string target;
  std::string version;
  std::unordered_map<std::string, std::string> headers;
  std::string body;

  bool is_get() const noexcept { return method == "GET"; }
  bool is_head() const noexcept { return method == "HEAD"; }
  bool is_post() const noexcept { return method == "POST"; }

  bool keep_alive() const noexcept {
    auto it = headers.find("connection");
    if (it != headers.end()) {
      const std::string value = to_lower(it->second);
      if (value.find("close") != std::string::npos) {
        return false;
      }
      if (value.find("keep-alive") != std::string::npos) {
        return true;
      }
    }
    return version == "HTTP/1.1";
  }
};

struct HttpResponse {
  int status = 200;
  std::string reason = "OK";
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  bool head_only = false;

  std::string to_wire() const {
    std::string wire = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n";
    bool has_content_length = false;
    for (const auto& header : headers) {
      if (to_lower(header.first) == "content-length") {
        has_content_length = true;
      }
      wire += header.first + ": " + header.second + "\r\n";
    }
    if (!has_content_length) {
      wire += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    wire += "\r\n";
    if (!head_only) {
      wire += body;
    }
    return wire;
  }
};

inline std::string mime_type_for(std::string_view path) {
  const auto dot = path.find_last_of('.');
  if (dot == std::string_view::npos) {
    return "text/plain; charset=utf-8";
  }
  const std::string ext = to_lower(path.substr(dot));
  if (ext == ".html" || ext == ".htm") {
    return "text/html; charset=utf-8";
  }
  if (ext == ".css") {
    return "text/css; charset=utf-8";
  }
  if (ext == ".js") {
    return "application/javascript; charset=utf-8";
  }
  if (ext == ".json") {
    return "application/json; charset=utf-8";
  }
  if (ext == ".png") {
    return "image/png";
  }
  if (ext == ".jpg" || ext == ".jpeg") {
    return "image/jpeg";
  }
  if (ext == ".gif") {
    return "image/gif";
  }
  if (ext == ".ico") {
    return "image/x-icon";
  }
  return "application/octet-stream";
}

inline bool path_is_safe(const std::filesystem::path& path) {
  for (const auto& part : path) {
    if (part == "..") {
      return false;
    }
  }
  return true;
}

}  // namespace cws
