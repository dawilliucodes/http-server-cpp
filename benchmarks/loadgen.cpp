#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

using clock_type = std::chrono::steady_clock;

constexpr size_t kBuckets = 200000;
constexpr int kMaxEvents = 256;

struct Options {
  int connections = 10;
  int duration = 30;
  int warmup = 10;
  int threads = 0;
  uint16_t port = 8080;
  std::string path = "/bench.txt";
  int server_pid = 0;
};

struct Histogram {
  std::vector<uint32_t> buckets{std::vector<uint32_t>(kBuckets, 0)};
  uint64_t overflow = 0;
  uint64_t count = 0;
  uint64_t max_us = 0;

  void add(uint64_t us) {
    ++count;
    max_us = std::max(max_us, us);
    if (us < kBuckets) ++buckets[us];
    else ++overflow;
  }

  void merge(const Histogram& other) {
    for (size_t i = 0; i < kBuckets; ++i) buckets[i] += other.buckets[i];
    overflow += other.overflow;
    count += other.count;
    max_us = std::max(max_us, other.max_us);
  }

  long long percentile(double fraction) const {
    if (count == 0) return -1;
    const uint64_t target = static_cast<uint64_t>(fraction * static_cast<double>(count));
    uint64_t seen = 0;
    for (size_t i = 0; i < kBuckets; ++i) {
      seen += buckets[i];
      if (seen >= target) return static_cast<long long>(i);
    }
    return -1;
  }
};

struct Connection {
  int fd = -1;
  std::string in;
  size_t sent = 0;
  clock_type::time_point started;
  bool timing = false;
};

std::atomic<uint64_t> g_errors{0};

bool set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

int dial(uint16_t port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = inet_addr("127.0.0.1");
  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }

  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  if (!set_nonblocking(fd)) {
    close(fd);
    return -1;
  }
  return fd;
}

size_t complete_length(const std::string& buf) {
  const size_t head = buf.find("\r\n\r\n");
  if (head == std::string::npos) return 0;

  size_t body = 0;
  const size_t at = buf.find("Content-Length:");
  if (at != std::string::npos && at < head) {
    body = std::strtoul(buf.c_str() + at + 15, nullptr, 10);
  }
  const size_t total = head + 4 + body;
  return buf.size() >= total ? total : 0;
}

void run_thread(const Options& opt, int connections, const std::string& request,
                clock_type::time_point measure_from, clock_type::time_point stop_at,
                Histogram& histogram) {
  int epoll_fd = epoll_create1(0);
  if (epoll_fd < 0) {
    ++g_errors;
    return;
  }

  std::vector<Connection> conns(static_cast<size_t>(connections));
  for (auto& conn : conns) {
    conn.fd = dial(opt.port);
    if (conn.fd < 0) {
      ++g_errors;
      continue;
    }
    conn.started = clock_type::now();
    conn.timing = clock_type::now() >= measure_from;

    epoll_event ev{};
    ev.events = EPOLLOUT;
    ev.data.ptr = &conn;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, conn.fd, &ev);
  }

  std::vector<epoll_event> events(kMaxEvents);
  while (clock_type::now() < stop_at) {
    int n = epoll_wait(epoll_fd, events.data(), kMaxEvents, 50);
    if (n < 0) {
      if (errno == EINTR) continue;
      ++g_errors;
      break;
    }

    for (int i = 0; i < n; ++i) {
      auto* conn = static_cast<Connection*>(events[i].data.ptr);
      if (conn->fd < 0) continue;

      if (events[i].events & (EPOLLHUP | EPOLLERR)) {
        ++g_errors;
        close(conn->fd);
        conn->fd = -1;
        continue;
      }

      if (events[i].events & EPOLLOUT) {
        ssize_t written = send(conn->fd, request.data() + conn->sent,
                               request.size() - conn->sent, MSG_NOSIGNAL);
        if (written < 0) {
          if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
          ++g_errors;
          close(conn->fd);
          conn->fd = -1;
          continue;
        }
        conn->sent += static_cast<size_t>(written);
        if (conn->sent < request.size()) continue;

        conn->started = clock_type::now();
        conn->timing = conn->started >= measure_from;
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.ptr = conn;
        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev);
        continue;
      }

      if (events[i].events & EPOLLIN) {
        char buf[16384];
        ssize_t got = recv(conn->fd, buf, sizeof(buf), 0);
        if (got <= 0) {
          if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
          ++g_errors;
          close(conn->fd);
          conn->fd = -1;
          continue;
        }
        conn->in.append(buf, static_cast<size_t>(got));

        const size_t total = complete_length(conn->in);
        if (total == 0) continue;

        if (conn->timing) {
          const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                                   clock_type::now() - conn->started)
                                   .count();
          histogram.add(static_cast<uint64_t>(elapsed));
        }
        conn->in.erase(0, total);
        conn->sent = 0;

        epoll_event ev{};
        ev.events = EPOLLOUT;
        ev.data.ptr = conn;
        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev);
      }
    }
  }

  for (auto& conn : conns) {
    if (conn.fd >= 0) close(conn.fd);
  }
  close(epoll_fd);
}

// utime+stime in seconds, from /proc/<pid>/stat fields 14 and 15
double cpu_seconds(int pid) {
  std::ifstream file("/proc/" + std::to_string(pid) + "/stat");
  if (!file) return -1;
  std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

  const size_t after_comm = text.rfind(')');
  if (after_comm == std::string::npos) return -1;

  std::vector<std::string> fields;
  size_t pos = after_comm + 2;
  while (pos < text.size()) {
    size_t space = text.find(' ', pos);
    if (space == std::string::npos) space = text.size();
    fields.push_back(text.substr(pos, space - pos));
    pos = space + 1;
  }
  if (fields.size() < 14) return -1;

  const long ticks = sysconf(_SC_CLK_TCK);
  return (std::strtod(fields[11].c_str(), nullptr) + std::strtod(fields[12].c_str(), nullptr)) /
         static_cast<double>(ticks);
}

long peak_rss_kb(int pid) {
  std::ifstream file("/proc/" + std::to_string(pid) + "/status");
  std::string key;
  while (file >> key) {
    if (key == "VmHWM:") {
      long value = 0;
      file >> value;
      return value;
    }
  }
  return -1;
}

Options parse(int argc, char** argv) {
  Options opt;
  for (int i = 1; i < argc - 1; ++i) {
    const std::string flag = argv[i];
    const char* value = argv[++i];
    if (flag == "--connections") opt.connections = std::atoi(value);
    else if (flag == "--duration") opt.duration = std::atoi(value);
    else if (flag == "--warmup") opt.warmup = std::atoi(value);
    else if (flag == "--threads") opt.threads = std::atoi(value);
    else if (flag == "--port") opt.port = static_cast<uint16_t>(std::atoi(value));
    else if (flag == "--path") opt.path = value;
    else if (flag == "--pid") opt.server_pid = std::atoi(value);
    else {
      std::cerr << "unknown option: " << flag << '\n';
      std::exit(2);
    }
  }
  return opt;
}

}

int main(int argc, char** argv) {
  const Options opt = parse(argc, argv);
  if (opt.connections < 1 || opt.duration < 1) {
    std::cerr << "usage: loadgen --connections N --duration S [--warmup S] [--threads T]\n";
    return 2;
  }

  int threads = opt.threads > 0 ? opt.threads : std::min(opt.connections, 6);
  threads = std::min(threads, opt.connections);

  const std::string request =
      "GET " + opt.path + " HTTP/1.1\r\nHost: bench\r\nConnection: keep-alive\r\n\r\n";

  const auto start = clock_type::now();
  const auto measure_from = start + std::chrono::seconds(opt.warmup);
  const auto stop_at = measure_from + std::chrono::seconds(opt.duration);

  std::vector<Histogram> histograms(static_cast<size_t>(threads));
  std::vector<std::thread> workers;
  workers.reserve(static_cast<size_t>(threads));

  const int base = opt.connections / threads;
  int spare = opt.connections % threads;
  for (int i = 0; i < threads; ++i) {
    const int share = base + (spare-- > 0 ? 1 : 0);
    workers.emplace_back(run_thread, std::cref(opt), share, std::cref(request), measure_from,
                         stop_at, std::ref(histograms[static_cast<size_t>(i)]));
  }
  std::this_thread::sleep_until(measure_from);
  const double cpu_before = opt.server_pid > 0 ? cpu_seconds(opt.server_pid) : -1;

  for (auto& worker : workers) worker.join();

  double cpu_after = opt.server_pid > 0 ? cpu_seconds(opt.server_pid) : -1;
  const long rss = opt.server_pid > 0 ? peak_rss_kb(opt.server_pid) : -1;

  Histogram total;
  for (const auto& histogram : histograms) total.merge(histogram);

  const double rps = static_cast<double>(total.count) / opt.duration;
  const double cpu = (cpu_before >= 0 && cpu_after >= 0) ? cpu_after - cpu_before : -1;

  std::printf("%d,%.0f,%lld,%lld,%lld,%lld,%llu,%llu,%llu,%.2f,%ld\n", opt.connections, rps,
              total.percentile(0.50), total.percentile(0.95), total.percentile(0.99),
              total.percentile(0.999), static_cast<unsigned long long>(total.max_us),
              static_cast<unsigned long long>(total.count),
              static_cast<unsigned long long>(g_errors.load()), cpu, rss);
  return 0;
}
