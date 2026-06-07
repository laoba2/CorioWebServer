#pragma once

#include "AsyncLogger.h"
#include "Task.h"
#include "Types.h"
#include "UringScheduler.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace cws {

class HttpServer {
 public:
  struct Config {
    uint16_t port{8080};
    std::size_t worker_count{4};
    std::size_t ring_entries{1024};
    std::filesystem::path docroot{};
    AsyncLogger* logger{nullptr};
    Ns request_timeout{std::chrono::seconds(30)};
  };

  explicit HttpServer(Config config);
  ~HttpServer();

  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  void start();
  void stop();
  void wait();

 private:
  struct ParsedRequest {
    HttpRequest request;
    std::size_t consumed{0};
  };

  Task<void> accept_loop();
  Task<void> handle_connection(UniqueFd client);
  Task<std::optional<HttpRequest>> read_request(int fd, std::string& buffer);
  Task<void> write_all(int fd, std::string payload);

  std::optional<ParsedRequest> try_parse_request(const std::string& buffer) const;
  HttpResponse build_response(const HttpRequest& request) const;
  std::optional<std::string> load_file(const std::filesystem::path& path) const;
  std::filesystem::path resolve_path(std::string target) const;
  static std::string strip_query(std::string target);
  static std::string fallback_favicon();
  UniqueFd create_listen_socket(uint16_t port);
  std::filesystem::path detect_docroot(std::filesystem::path configured) const;

  Config config_;
  UniqueFd listen_fd_;
  std::unique_ptr<UringScheduler> acceptor_;
  std::vector<std::unique_ptr<UringScheduler>> workers_;
  std::vector<std::thread> threads_;
  std::thread accept_thread_;
  std::atomic_bool running_{false};
  std::atomic_size_t next_worker_{0};
};

}  // namespace cws
