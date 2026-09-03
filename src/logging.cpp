#include "logging.h"

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace httpserver {

namespace {

std::mutex g_log_mutex;

constexpr size_t kMaxBuffered = 4 * 1024 * 1024;  // per worker, then lines are dropped
constexpr auto kFlushInterval = std::chrono::milliseconds(50);

struct Sink {
  std::mutex mutex;
  std::string pending;
  uint64_t dropped = 0;
};

std::mutex g_sinks_mutex;
std::vector<std::shared_ptr<Sink>> g_sinks;
thread_local std::shared_ptr<Sink> t_sink;

std::ofstream g_access_file;  // closed = stdout
std::thread g_writer;
std::mutex g_writer_mutex;
std::condition_variable g_writer_cv;
bool g_writer_stop = false;

// the registry holds a reference, so a sink outlives the thread that filled it
Sink& sink_for_this_thread() {
  if (!t_sink) {
    t_sink = std::make_shared<Sink>();
    std::lock_guard<std::mutex> lock(g_sinks_mutex);
    g_sinks.push_back(t_sink);
  }
  return *t_sink;
}

void append_timestamp(std::string& out) {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const auto secs = time_point_cast<seconds>(now);
  const auto micros = duration_cast<microseconds>(now - secs).count();

  const std::time_t t = system_clock::to_time_t(secs);
  std::tm tm{};
  gmtime_r(&t, &tm);

  char buf[40];
  size_t len = std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
  len += static_cast<size_t>(
      std::snprintf(buf + len, sizeof(buf) - len, ".%06lldZ", static_cast<long long>(micros)));
  out.append(buf, len);
}

void flush_batch() {
  std::vector<std::shared_ptr<Sink>> sinks;
  {
    std::lock_guard<std::mutex> lock(g_sinks_mutex);
    sinks = g_sinks;
  }

  std::string batch;
  uint64_t dropped = 0;
  for (const auto& sink : sinks) {
    std::string chunk;
    {
      std::lock_guard<std::mutex> lock(sink->mutex);
      chunk.swap(sink->pending);  // O(1), and no syscall inside the lock
      dropped += sink->dropped;
      sink->dropped = 0;
    }
    if (batch.empty()) batch = std::move(chunk);
    else batch += chunk;
  }

  if (dropped > 0) batch += "# " + std::to_string(dropped) + " access log lines dropped\n";
  if (batch.empty()) return;

  if (g_access_file.is_open()) g_access_file << batch << std::flush;
  else std::cout << batch << std::flush;
}

void writer_loop() {
  while (true) {
    {
      std::unique_lock<std::mutex> lock(g_writer_mutex);
      g_writer_cv.wait_for(lock, kFlushInterval, [] { return g_writer_stop; });
      if (g_writer_stop) break;
    }
    flush_batch();
  }
  flush_batch();  // whatever the workers left behind
}

}

bool fail(const char* what) {
  int err = errno;
  std::lock_guard<std::mutex> lock(g_log_mutex);
  std::cerr << what << ": " << std::strerror(err) << '\n';
  return false;
}

void log_line(const std::string& message) {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  std::cerr << message << '\n';
}

bool open_access_log(const std::string& path) {
  if (!path.empty()) {
    g_access_file.open(path, std::ios::app);
    if (!g_access_file) {
      std::cerr << "could not open log file " << path << ": " << std::strerror(errno) << '\n';
      return false;
    }
  }
  g_writer = std::thread(writer_loop);
  return true;
}

void log_access(const AccessRecord& record) {
  thread_local std::string line;
  line.clear();

  append_timestamp(line);
  line += ' ';
  line += record.client_ip;
  line += " \"";
  line += record.method;
  line += ' ';
  line += record.path;
  line += "\" ";
  line += std::to_string(record.status);
  line += ' ';
  line += std::to_string(record.bytes_sent);
  line += ' ';
  line += std::to_string(record.micros);
  line += "us\n";

  Sink& sink = sink_for_this_thread();
  std::lock_guard<std::mutex> lock(sink.mutex);
  if (sink.pending.size() + line.size() > kMaxBuffered) {
    ++sink.dropped;  
    return;
  }
  sink.pending += line;
}

void close_access_log() {
  if (g_writer.joinable()) {
    {
      std::lock_guard<std::mutex> lock(g_writer_mutex);
      g_writer_stop = true;
    }
    g_writer_cv.notify_one();
    g_writer.join();
  }
  if (g_access_file.is_open()) g_access_file.close();
}

}
