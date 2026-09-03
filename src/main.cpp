#include <cerrno>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <system_error>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include "config.h"
#include "logging.h"
#include "static_file.h"
#include "worker.h"

namespace fs = std::filesystem;
using namespace httpserver;

namespace {

constexpr int kBacklog = SOMAXCONN;

int make_listener(uint16_t port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    fail("socket");
    return -1;
  }

  int opt = 1;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) fail("setsockopt");
  else if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) fail("bind");
  else if (listen(fd, kBacklog) < 0) fail("listen");
  else if (set_nonblocking(fd)) return fd;

  close(fd);
  return -1;
}

int make_signal_fd() {
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGTERM);
  if (pthread_sigmask(SIG_BLOCK, &mask, nullptr) != 0) {
    fail("pthread_sigmask");
    return -1;
  }

  int fd = signalfd(-1, &mask, SFD_NONBLOCK);
  if (fd < 0) fail("signalfd");
  return fd;
}

std::string ip_of(const sockaddr_in& peer) {
  char buf[INET_ADDRSTRLEN];
  if (inet_ntop(AF_INET, &peer.sin_addr, buf, sizeof(buf)) == nullptr) return "-";
  return buf;
}

}

int main(int argc, char** argv) {
  int exit_code = 0;
  auto parsed = parse_args(argc, argv, exit_code);
  if (!parsed) return exit_code;
  Config config = *parsed;

  std::error_code ec;
  config.doc_root = fs::weakly_canonical(config.doc_root, ec);

  const int doc_root_fd = open(config.doc_root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (doc_root_fd < 0) {
    std::cerr << "cannot open document root " << config.doc_root << ": " << std::strerror(errno)
              << '\n';
    return 1;
  }
  if (!resolve_beneath_supported(doc_root_fd)) {
    std::cerr << "kernel lacks openat2(RESOLVE_BENEATH); needs Linux 5.6 or newer\n";
    return 1;
  }

  if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
    fail("signal(SIGPIPE)");
    return 1;
  }

  const int listen_fd = make_listener(config.port);
  if (listen_fd < 0) return 1;

  const int signal_fd = make_signal_fd();
  if (signal_fd < 0) return 1;

  if (!open_access_log(config.log_path)) return 1;

  unsigned int worker_count = config.workers;
  if (worker_count == 0) {
    worker_count = std::thread::hardware_concurrency();
    if (worker_count == 0) worker_count = 4;
  }

  std::vector<std::unique_ptr<Worker>> workers;
  workers.reserve(worker_count);
  for (unsigned int i = 0; i < worker_count; ++i) {
    auto worker = std::make_unique<Worker>(doc_root_fd);
    if (!worker->init()) return 1;
    worker->start();
    workers.push_back(std::move(worker));
  }

  std::cerr << "listening on http://localhost:" << config.port << " (doc root: " << config.doc_root
            << ", " << worker_count << " event-loop workers, log: "
            << (config.log_path.empty() ? "stdout" : config.log_path) << ")\n";

  int accept_epoll = epoll_create1(0);
  if (accept_epoll < 0) {
    fail("epoll_create1(accept)");
    return 1;
  }
  for (int fd : {listen_fd, signal_fd}) {
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    if (epoll_ctl(accept_epoll, EPOLL_CTL_ADD, fd, &ev) < 0) {
      fail("epoll_ctl(accept)");
      return 1;
    }
  }

  size_t next_worker = 0;
  auto accept_all = [&] {
    while (true) {
      sockaddr_in peer{};
      socklen_t peer_len = sizeof(peer);
      int conn = accept(listen_fd, reinterpret_cast<sockaddr*>(&peer), &peer_len);
      if (conn < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) fail("accept");
        return;
      }
      if (!set_nonblocking(conn)) {
        close(conn);
        continue;
      }
      workers[next_worker]->hand_off(conn, ip_of(peer));
      next_worker = (next_worker + 1) % workers.size();
    }
  };

  bool running = true;
  while (running) {
    epoll_event events[4];
    int n = epoll_wait(accept_epoll, events, 4, -1);
    if (n < 0) {
      if (errno == EINTR) continue;
      fail("epoll_wait(accept)");
      break;
    }

    for (int i = 0; i < n && running; ++i) {
      if (events[i].data.fd == signal_fd) {
        signalfd_siginfo info{};
        if (read(signal_fd, &info, sizeof(info)) < 0) fail("read(signalfd)");
        std::cerr << "caught signal " << info.ssi_signo << ", shutting down\n";
        running = false;
      } else {
        accept_all();
      }
    }
  }

  accept_all();

  close(listen_fd);
  for (auto& worker : workers) worker->stop();
  for (auto& worker : workers) worker->join();

  close(signal_fd);
  close(accept_epoll);
  close(doc_root_fd);
  close_access_log();
  std::cerr << "shutdown complete\n";
  return 0;
}
