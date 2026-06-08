#include "HttpServer.h"

#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdexcept>
#include <system_error>

namespace cws {

namespace fs = std::filesystem;

HttpServer::HttpServer(Config config)
    : config_(std::move(config)),
      listen_fd_(create_listen_socket(config_.port)) {
  config_.docroot = detect_docroot(std::move(config_.docroot));
  if (config_.worker_count == 0) {
    config_.worker_count = 1;
  }
}

HttpServer::~HttpServer() {
  stop();
  wait();
}

void HttpServer::start() {
  if (running_.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  if (config_.logger) {
    config_.logger->info("starting coroutine web server on port {}", config_.port);
  }

  acceptor_ = std::make_unique<UringScheduler>(config_.ring_entries);
  workers_.reserve(config_.worker_count);
  for (std::size_t i = 0; i < config_.worker_count; ++i) {
    workers_.push_back(std::make_unique<UringScheduler>(config_.ring_entries));
  }

  for (std::size_t i = 0; i < workers_.size(); ++i) {
    threads_.emplace_back([this, i]() { workers_[i]->run(); });
  }
  accept_thread_ = std::thread([this]() { acceptor_->run(); });

  std::move(accept_loop()).start(*acceptor_);
}

void HttpServer::stop() {
  if (!running_.exchange(false, std::memory_order_acq_rel)) {
    return;
  }
  if (config_.logger) {
    config_.logger->info("stopping coroutine web server");
  }
  if (acceptor_) {
    acceptor_->stop();
  }
  for (auto& worker : workers_) {
    if (worker) {
      worker->stop();
    }
  }
}

void HttpServer::wait() {
  if (accept_thread_.joinable()) {
    accept_thread_.join();
  }
  for (auto& thread : threads_) {
    if (thread.joinable()) {
      thread.join();
    }
  }
  threads_.clear();
}

UniqueFd HttpServer::create_listen_socket(uint16_t port) {
  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    throw std::runtime_error(std::string("socket failed: ") + std::strerror(errno));
  }
  int yes = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#ifdef SO_REUSEPORT
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    const auto error = std::string("bind failed: ") + std::strerror(errno);
    ::close(fd);
    throw std::runtime_error(error);
  }
  if (::listen(fd, SOMAXCONN) < 0) {
    const auto error = std::string("listen failed: ") + std::strerror(errno);
    ::close(fd);
    throw std::runtime_error(error);
  }
  return UniqueFd(fd);
}

std::filesystem::path HttpServer::detect_docroot(std::filesystem::path configured) const {
  if (!configured.empty()) {
    return configured;
  }
  std::error_code ec;
  auto current = fs::current_path(ec);
  if (ec) {
    return fs::current_path();
  }
  auto probe = current;
  for (int i = 0; i < 4; ++i) {
    if (fs::exists(probe / "datum" / "WebServer.png")) {
      return probe;
    }
    if (!probe.has_parent_path()) {
      break;
    }
    probe = probe.parent_path();
  }
  return current;
}

Task<void> HttpServer::accept_loop() {
  auto* scheduler = UringScheduler::current();
  if (!scheduler) {
    throw std::runtime_error("accept loop must run on a scheduler thread");
  }

  while (running_.load(std::memory_order_acquire)) {
    sockaddr_storage addr{};
    socklen_t addrlen = sizeof(addr);
    try {
      const int client_fd = co_await scheduler->async_accept(listen_fd_.get(), reinterpret_cast<sockaddr*>(&addr),
                                                             &addrlen, SOCK_NONBLOCK | SOCK_CLOEXEC, false);
      if (client_fd < 0) {
        continue;
      }
      if (config_.logger) {
        config_.logger->info("accepted client fd {}", client_fd);
      }
      const std::size_t index = next_worker_.fetch_add(1, std::memory_order_relaxed) % workers_.size();
      auto* worker = workers_[index].get();
      worker->post([this, worker, client_fd]() mutable {
        std::move(handle_connection(UniqueFd(client_fd))).start(*worker);
      });
    } catch (const std::exception& e) {
      if (!running_.load(std::memory_order_acquire)) {
        break;
      }
      if (config_.logger) {
        config_.logger->warn("accept loop error: {}", e.what());
      }
    }
  }
  co_return;
}

std::string HttpServer::strip_query(std::string target) {
  if (const auto pos = target.find('?'); pos != std::string::npos) {
    target.resize(pos);
  }
  return target;
}

std::filesystem::path HttpServer::resolve_path(std::string target) const {
  target = strip_query(std::move(target));
  if (target.empty() || target == "/") {
    target = "/index.html";
  }
  if (target == "/hello") {
    return {};
  }
  if (target == "/favicon.ico") {
    const auto candidate_png = config_.docroot / "datum" / "WebServer.png";
    if (fs::exists(candidate_png)) {
      return candidate_png;
    }
    const auto candidate_ico = config_.docroot / "favicon.ico";
    if (fs::exists(candidate_ico)) {
      return candidate_ico;
    }
    return candidate_png;
  }
  if (!target.empty() && target.front() == '/') {
    target.erase(target.begin());
  }
  const fs::path relative = fs::path(target).lexically_normal();
  if (!path_is_safe(relative)) {
    return {};
  }
  return config_.docroot / relative;
}

std::optional<std::string> HttpServer::load_file(const std::filesystem::path& path) const {
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return std::nullopt;
  }
  UniqueFd file(fd);

  struct stat st {};
  if (::fstat(file.get(), &st) != 0 || !S_ISREG(st.st_mode)) {
    return std::nullopt;
  }
  if (st.st_size == 0) {
    return std::string();
  }

  void* mapped = ::mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, file.get(), 0);
  if (mapped == MAP_FAILED) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      return std::nullopt;
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }
  std::string body(static_cast<const char*>(mapped), static_cast<size_t>(st.st_size));
  ::munmap(mapped, static_cast<size_t>(st.st_size));
  return body;
}

HttpResponse HttpServer::build_response(const HttpRequest& request) const {
  HttpResponse response;
  response.headers.emplace_back("Server", "CoroutineWebServer");
  response.headers.emplace_back("Connection", request.keep_alive() ? "keep-alive" : "close");

  if (request.is_post()) {
    response.body = request.body;
    response.headers.emplace_back("Content-Type", "text/plain; charset=utf-8");
    response.head_only = request.is_head();
    return response;
  }

  if (request.target == "/hello") {
    response.body = "Hello from CoroutineWebServer\n";
    response.headers.emplace_back("Content-Type", "text/plain; charset=utf-8");
    response.head_only = request.is_head();
    return response;
  }

  const fs::path resolved = resolve_path(request.target);
  if (resolved.empty() && request.target != "/hello") {
    response.status = 400;
    response.reason = "Bad Request";
    response.body = "Bad Request\n";
    response.headers.emplace_back("Content-Type", "text/plain; charset=utf-8");
    response.head_only = request.is_head();
    return response;
  }

  if (!resolved.empty()) {
    auto body = load_file(resolved);
    if (!body.has_value()) {
      if (request.target == "/favicon.ico") {
        response.body = fallback_favicon();
        response.headers.emplace_back("Content-Type", "text/plain; charset=utf-8");
        response.head_only = request.is_head();
        return response;
      }
      response.status = 404;
      response.reason = "Not Found";
      response.body = "Not Found\n";
      response.headers.emplace_back("Content-Type", "text/plain; charset=utf-8");
      response.head_only = request.is_head();
      return response;
    }

    response.body = std::move(*body);
    response.headers.emplace_back("Content-Type", mime_type_for(resolved.string()));
    response.head_only = request.is_head();
    return response;
  }

  response.body = fallback_favicon();
  response.headers.emplace_back("Content-Type", "text/plain; charset=utf-8");
  response.head_only = request.is_head();
  return response;
}

std::string HttpServer::fallback_favicon() {
  return "favicon";
}

std::optional<HttpServer::ParsedRequest> HttpServer::try_parse_request(const std::string& buffer) const {
  const auto header_end = buffer.find("\r\n\r\n");
  if (header_end == std::string::npos) {
    return std::nullopt;
  }

  const std::string_view header_block(buffer.data(), header_end + 2);
  const std::size_t first_line_end = header_block.find("\r\n");
  if (first_line_end == std::string::npos) {
    return std::nullopt;
  }

  std::string request_line(header_block.substr(0, first_line_end));
  std::size_t pos = 0;
  auto next_token = [&request_line, &pos]() -> std::string {
    while (pos < request_line.size() && request_line[pos] == ' ') {
      ++pos;
    }
    const std::size_t begin = pos;
    while (pos < request_line.size() && request_line[pos] != ' ') {
      ++pos;
    }
    return request_line.substr(begin, pos - begin);
  };

  ParsedRequest parsed;
  parsed.request.method = next_token();
  parsed.request.target = next_token();
  parsed.request.version = next_token();

  if (parsed.request.method.empty() || parsed.request.target.empty() || parsed.request.version.empty()) {
    return std::nullopt;
  }
  parsed.request.target = strip_query(std::move(parsed.request.target));

  std::size_t line_begin = first_line_end + 2;
  while (line_begin < header_end) {
    const auto line_end = buffer.find("\r\n", line_begin);
    if (line_end == std::string::npos || line_end > header_end) {
      break;
    }
    if (line_end == line_begin) {
      break;
    }
    const std::string line = buffer.substr(line_begin, line_end - line_begin);
    const auto colon = line.find(':');
    if (colon != std::string::npos) {
      const std::string key = to_lower(trim(std::string_view(line).substr(0, colon)));
      const std::string value = trim(std::string_view(line).substr(colon + 1));
      parsed.request.headers.emplace(key, value);
    }
    line_begin = line_end + 2;
  }

  std::size_t body_length = 0;
  if (const auto it = parsed.request.headers.find("content-length"); it != parsed.request.headers.end()) {
    try {
      body_length = static_cast<std::size_t>(std::stoul(it->second));
    } catch (...) {
      return std::nullopt;
    }
  }

  const std::size_t body_begin = header_end + 4;
  if (buffer.size() < body_begin + body_length) {
    return std::nullopt;
  }
  parsed.request.body = buffer.substr(body_begin, body_length);
  parsed.consumed = body_begin + body_length;
  return parsed;
}

Task<std::optional<HttpRequest>> HttpServer::read_request(int fd, std::string& buffer) {
  auto* scheduler = UringScheduler::current();
  if (!scheduler) {
    throw std::runtime_error("read_request must run on a scheduler thread");
  }

  std::array<char, 8192> chunk{};
  while (running_.load(std::memory_order_acquire)) {
    if (auto parsed = try_parse_request(buffer); parsed.has_value()) {
      buffer.erase(0, parsed->consumed);
      co_return parsed->request;
    }

    try {
      const int received =
          co_await scheduler->async_recv_for(fd, chunk.data(), chunk.size(), config_.request_timeout, 0);
      if (received <= 0) {
        co_return std::nullopt;
      }
      buffer.append(chunk.data(), static_cast<std::size_t>(received));
    } catch (const std::system_error& e) {
      if (e.code().value() == ETIMEDOUT || e.code().value() == ECANCELED) {
        co_return std::nullopt;
      }
      throw;
    }
  }
  co_return std::nullopt;
}

Task<void> HttpServer::write_all(int fd, std::string payload) {
  auto* scheduler = UringScheduler::current();
  if (!scheduler) {
    throw std::runtime_error("write_all must run on a scheduler thread");
  }

  std::size_t offset = 0;
  while (offset < payload.size() && running_.load(std::memory_order_acquire)) {
    const int written =
        co_await scheduler->async_send(fd, payload.data() + offset, payload.size() - offset, 0);
    if (written <= 0) {
      co_return;
    }
    offset += static_cast<std::size_t>(written);
  }
  co_return;
}

Task<void> HttpServer::handle_connection(UniqueFd client) {
  std::string buffer;
  buffer.reserve(8192);

  try {
    while (running_.load(std::memory_order_acquire) && client) {
      auto request = co_await read_request(client.get(), buffer);
      if (!request.has_value()) {
        break;
      }

      if (config_.logger) {
        config_.logger->info("request {} {}", request->method, request->target);
      }

      HttpResponse response = build_response(*request);
      std::string wire = response.to_wire();
      co_await write_all(client.get(), std::move(wire));

      if (!request->keep_alive()) {
        break;
      }
    }
  } catch (const std::exception& e) {
    if (config_.logger) {
      config_.logger->error("connection error: {}", e.what());
    }
  }
  co_return;
}

}  // namespace cws
