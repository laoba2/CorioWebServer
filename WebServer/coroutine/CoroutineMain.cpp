#include "AsyncLogger.h"
#include "HttpServer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

namespace {

std::atomic_bool g_stop{false};

void on_signal(int) {
  g_stop.store(true, std::memory_order_release);
}

}  // namespace

int main(int argc, char* argv[]) {
  uint16_t port = 8080;
  std::size_t threads = std::max<std::size_t>(4, std::thread::hardware_concurrency());
  std::string log_path = "/tmp/coroutine-webserver.log";
  std::filesystem::path docroot;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-p" && i + 1 < argc) {
      port = static_cast<uint16_t>(std::stoi(argv[++i]));
    } else if (arg == "-t" && i + 1 < argc) {
      threads = static_cast<std::size_t>(std::stoul(argv[++i]));
    } else if (arg == "-l" && i + 1 < argc) {
      log_path = argv[++i];
    } else if (arg == "-d" && i + 1 < argc) {
      docroot = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "usage: " << argv[0] << " [-t threads] [-p port] [-l log_path] [-d docroot]\n";
      return 0;
    }
  }

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  std::signal(SIGPIPE, SIG_IGN);

  cws::AsyncLogger logger(std::move(log_path));
  logger.start();

  cws::HttpServer::Config config;
  config.port = port;
  config.worker_count = threads;
  config.docroot = std::move(docroot);
  config.logger = &logger;
  config.request_timeout = std::chrono::seconds(30);

  cws::HttpServer server(std::move(config));
  server.start();

  logger.info("CoroutineWebServer started on port {}", port);
  while (!g_stop.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  server.stop();
  server.wait();
  logger.info("CoroutineWebServer stopped");
  logger.stop();
  return 0;
}
