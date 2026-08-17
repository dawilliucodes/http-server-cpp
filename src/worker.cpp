#include "worker.h"

#include <cerrno>
#include <chrono>
#include <exception>
#include <utility>

#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "http_response.h"
#include "logging.h"
#include "request_handler.h"

namespace httpserver {

namespace {

constexpr size_t kMaxRequestSize = 8192;  // headers only, no bodies yet
constexpr int kMaxEvents = 128;
constexpr size_t kReadBlock = 4096;

constexpr auto kIdleTimeout = std::chrono::seconds(10);
constexpr long kSweepIntervalSeconds = 1;

constexpr auto kDrainGrace = std::chrono::milliseconds(500);
constexpr int kDrainPollMs = 20;

bool would_block() { return errno == EAGAIN || errno == EWOULDBLOCK; }

}

bool set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return fail("fcntl(F_GETFL)");
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return fail("fcntl(F_SETFL)");
  return true;
}

Worker::Worker(int doc_root_fd) : doc_root_fd_(doc_root_fd) {}

Worker::~Worker() {
  join();
  for (const auto& [fd, state] : connections_) close(fd);
  for (int fd : {epoll_fd_, event_fd_, timer_fd_}) {
    if (fd >= 0) close(fd);
  }
}

bool Worker::watch(int op, int fd, uint32_t events, const char* what) {
  epoll_event ev{};
  ev.events = events;
  ev.data.fd = fd;
  if (epoll_ctl(epoll_fd_, op, fd, &ev) < 0) return fail(what);
  return true;
}

bool Worker::init() {
  epoll_fd_ = epoll_create1(0);
  if (epoll_fd_ < 0) return fail("epoll_create1");

  event_fd_ = eventfd(0, EFD_NONBLOCK);
  if (event_fd_ < 0) return fail("eventfd");
  if (!watch(EPOLL_CTL_ADD, event_fd_, EPOLLIN, "epoll_ctl(eventfd)")) return false;

  timer_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
  if (timer_fd_ < 0) return fail("timerfd_create");

  itimerspec spec{};
  spec.it_interval.tv_sec = kSweepIntervalSeconds;
  spec.it_value.tv_sec = kSweepIntervalSeconds;
  if (timerfd_settime(timer_fd_, 0, &spec, nullptr) < 0) return fail("timerfd_settime");

  return watch(EPOLL_CTL_ADD, timer_fd_, EPOLLIN, "epoll_ctl(timerfd)");
}

void Worker::start() { thread_ = std::thread(&Worker::run, this); }

void Worker::join() {
  if (thread_.joinable()) thread_.join();
}

void Worker::wake() {
  uint64_t one = 1;
  if (write(event_fd_, &one, sizeof(one)) < 0) fail("write(eventfd)");
}

void Worker::stop() {
  stopping_.store(true);
  wake();
}

void Worker::hand_off(int fd, std::string client_ip) {
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    incoming_.push_back({fd, std::move(client_ip)});
  }
  wake();
}

void Worker::run() {
  std::vector<epoll_event> events(kMaxEvents);
  bool draining = false;
  std::chrono::steady_clock::time_point deadline;

  while (true) {
    if (stopping_.load() && !draining) {
      draining = true;
      deadline = std::chrono::steady_clock::now() + kDrainGrace;
    }

    if (draining) {
      if (connections_.empty()) break;
      if (std::chrono::steady_clock::now() >= deadline) {
        std::vector<int> left;
        left.reserve(connections_.size());
        for (const auto& [fd, state] : connections_) left.push_back(fd);
        for (int fd : left) discard_and_close(fd);
        break;
      }
    }

    int n = epoll_wait(epoll_fd_, events.data(), kMaxEvents, draining ? kDrainPollMs : -1);
    if (n < 0) {
      if (errno != EINTR) fail("epoll_wait");
      continue;
    }

    for (int i = 0; i < n; ++i) {
      const int fd = events[i].data.fd;
      const uint32_t ready = events[i].events;

      if (fd == event_fd_) {
        take_incoming();
      } else if (fd == timer_fd_) {
        sweep_idle();
      } else if (ready & (EPOLLHUP | EPOLLERR)) {
        close_connection(fd);
      } else if (ready & EPOLLIN) {
        handle_readable(fd);
      } else if (ready & EPOLLOUT) {
        handle_writable(fd);
      }
    }
  }
}

void Worker::take_incoming() {
  uint64_t ticks;
  if (read(event_fd_, &ticks, sizeof(ticks)) < 0 && !would_block()) fail("read(eventfd)");

  std::vector<Incoming> arrivals;
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    arrivals.swap(incoming_);
  }

  for (auto& arrival : arrivals) {
    if (!watch(EPOLL_CTL_ADD, arrival.fd, EPOLLIN, "epoll_ctl(add)")) {
      close(arrival.fd);
      continue;
    }
    connections_[arrival.fd].client_ip = std::move(arrival.client_ip);
  }
}

void Worker::sweep_idle() {
  uint64_t ticks;
  if (read(timer_fd_, &ticks, sizeof(ticks)) < 0 && !would_block()) fail("read(timerfd)");

  const auto now = std::chrono::steady_clock::now();
  std::vector<int> expired;
  for (const auto& [fd, state] : connections_) {
    if (state.stage == Stage::kReading && now - state.last_activity >= kIdleTimeout) {
      expired.push_back(fd);
    }
  }

  for (int fd : expired) {
    auto it = connections_.find(fd);
    if (it == connections_.end()) continue;
    if (it->second.read_buffer.empty()) {
      close_connection(fd);
    } else {
      queue_error(fd, it->second, 408, "Request Timeout");
    }
  }
}

void Worker::handle_readable(int fd) {
  auto it = connections_.find(fd);
  if (it == connections_.end()) return;

  char chunk[kReadBlock];
  ssize_t n = read(fd, chunk, sizeof(chunk));
  if (n < 0) {
    if (would_block()) return;
    fail("read");
    close_connection(fd);
    return;
  }
  if (n == 0) {
    close_connection(fd);
    return;
  }

  ConnectionState& state = it->second;
  state.read_buffer.append(chunk, static_cast<size_t>(n));
  state.last_activity = std::chrono::steady_clock::now();
  process_buffered(fd, state);
}

void Worker::process_buffered(int fd, ConnectionState& state) {
  const size_t marker = state.read_buffer.find("\r\n\r\n");
  const bool too_big = marker == std::string::npos ? state.read_buffer.size() > kMaxRequestSize
                                                   : marker + 4 > kMaxRequestSize;
  if (too_big) {
    queue_error(fd, state, 413, "Payload Too Large");
    return;
  }
  if (marker == std::string::npos) return;

  state.request_started = std::chrono::steady_clock::now();

  const std::string_view header_text(state.read_buffer.data(), marker + 2);

  Response response;
  try {
    response = handle_request(header_text, doc_root_fd_, !stopping_.load());
  } catch (const std::exception& e) {
    log_line(std::string("request handler threw: ") + e.what());
    response.text = build_error_response(500, "Internal Server Error");
    response.status = 500;
  } catch (...) {
    log_line("request handler threw a non-std exception");
    response.text = build_error_response(500, "Internal Server Error");
    response.status = 500;
  }

  state.read_buffer.erase(0, marker + 4);
  state.write_buffer = std::move(response.text);
  state.write_offset = 0;
  state.keep_alive = response.keep_alive;
  state.status = response.status;
  state.method = std::move(response.method);
  state.path = std::move(response.path);
  switch_to(fd, state, EPOLLOUT, Stage::kWriting);
}

void Worker::queue_error(int fd, ConnectionState& state, int status, std::string_view reason) {
  state.request_started = std::chrono::steady_clock::now();
  state.status = status;
  state.method = "-";
  state.path = "-";
  state.write_buffer = build_error_response(status, reason);
  state.write_offset = 0;
  state.keep_alive = false;
  switch_to(fd, state, EPOLLOUT, Stage::kWriting);
}

bool Worker::switch_to(int fd, ConnectionState& state, uint32_t events, Stage stage) {
  if (!watch(EPOLL_CTL_MOD, fd, events, "epoll_ctl(mod)")) {
    close_connection(fd);
    return false;
  }
  state.stage = stage;
  return true;
}

void Worker::handle_writable(int fd) {
  auto it = connections_.find(fd);
  if (it == connections_.end()) return;

  ConnectionState& state = it->second;
  const std::string& data = state.write_buffer;
  ssize_t n = write(fd, data.data() + state.write_offset, data.size() - state.write_offset);
  if (n < 0) {
    if (would_block()) return;
    fail("write");
    close_connection(fd);
    return;
  }

  state.write_offset += static_cast<size_t>(n);
  if (state.write_offset < data.size()) return;

  log_request(state);
  if (!state.keep_alive) {
    close_connection(fd);
    return;
  }

  state.write_buffer.clear();
  state.write_offset = 0;
  state.last_activity = std::chrono::steady_clock::now();
  if (!switch_to(fd, state, EPOLLIN, Stage::kReading)) return;

  // level-triggered epoll won't wake us again for a request already buffered
  process_buffered(fd, state);
}

void Worker::log_request(const ConnectionState& state) {
  if (state.status == 0) return;

  AccessRecord record;
  // both arms must be string_view, or the ternary yields a dangling temporary
  record.client_ip =
      state.client_ip.empty() ? std::string_view("-") : std::string_view(state.client_ip);
  record.method = state.method;
  record.path = state.path;
  record.status = state.status;
  record.bytes_sent = state.write_buffer.size();
  record.micros = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - state.request_started)
                      .count();
  log_access(record);
}

void Worker::close_connection(int fd) {
  close(fd);
  connections_.erase(fd);
}

void Worker::discard_and_close(int fd) {
  // closing with bytes unread makes the kernel send RST instead of FIN
  char scratch[kReadBlock];
  for (int i = 0; i < 4; ++i) {
    if (read(fd, scratch, sizeof(scratch)) <= 0) break;
  }
  close_connection(fd);
}

}
