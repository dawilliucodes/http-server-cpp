#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "connection_state.h"

namespace httpserver {

bool set_nonblocking(int fd);

// one epoll loop on one thread. a connection belongs to the worker it was handed
// to for its whole life, so none of the per-connection state is shared.
class Worker {
 public:
  explicit Worker(std::filesystem::path doc_root);
  ~Worker();

  Worker(const Worker&) = delete;
  Worker& operator=(const Worker&) = delete;

  bool init();
  void start();
  void stop();  // thread-safe
  void join();
  void hand_off(int fd, std::string client_ip);  // thread-safe

 private:
  void run();
  void wake();
  bool watch(int op, int fd, uint32_t events, const char* what);
  bool switch_to(int fd, ConnectionState& state, uint32_t events, Stage stage);

  void take_incoming();
  void sweep_idle();
  void handle_readable(int fd);
  void handle_writable(int fd);
  void process_buffered(int fd, ConnectionState& state);
  void queue_error(int fd, ConnectionState& state, int status, std::string_view reason);
  void log_request(const ConnectionState& state);
  void close_connection(int fd);
  void discard_and_close(int fd);

  int epoll_fd_ = -1;
  int event_fd_ = -1;
  int timer_fd_ = -1;
  std::filesystem::path doc_root_;
  std::unordered_map<int, ConnectionState> connections_;

  std::thread thread_;
  std::atomic<bool> stopping_{false};

  struct Incoming {
    int fd;
    std::string client_ip;
  };
  std::mutex queue_mutex_;
  std::vector<Incoming> incoming_;
};

}
