#include "logging.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <mutex>

namespace httpserver {

namespace {

std::mutex g_log_mutex;

std::mutex g_access_mutex;
std::ofstream g_access_file;  // closed = stdout

std::string timestamp_now() {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const auto secs = time_point_cast<seconds>(now);
  const auto micros = duration_cast<microseconds>(now - secs).count();

  const std::time_t t = system_clock::to_time_t(secs);
  std::tm tm{};
  gmtime_r(&t, &tm);

  char buf[40];
  size_t len = std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
  std::snprintf(buf + len, sizeof(buf) - len, ".%06lldZ", static_cast<long long>(micros));
  return buf;
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
  if (path.empty()) return true;

  std::lock_guard<std::mutex> lock(g_access_mutex);
  g_access_file.open(path, std::ios::app);
  if (!g_access_file) {
    std::cerr << "could not open log file " << path << ": " << std::strerror(errno) << '\n';
    return false;
  }
  return true;
}

void log_access(const AccessRecord& record) {
  std::string line = timestamp_now();
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

  std::lock_guard<std::mutex> lock(g_access_mutex);
  if (g_access_file.is_open()) {
    g_access_file << line << std::flush;
  } else {
    std::cout << line << std::flush;
  }
}

void close_access_log() {
  std::lock_guard<std::mutex> lock(g_access_mutex);
  if (g_access_file.is_open()) g_access_file.close();
}

}
