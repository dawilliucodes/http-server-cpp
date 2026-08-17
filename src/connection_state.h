#pragma once

#include <chrono>
#include <string>

namespace httpserver {

enum class Stage { kReading, kWriting };

struct ConnectionState {
  std::string read_buffer;
  std::string write_buffer;
  size_t write_offset = 0;
  Stage stage = Stage::kReading;
  bool keep_alive = false;
  std::chrono::steady_clock::time_point last_activity = std::chrono::steady_clock::now();

  std::string client_ip;
  std::chrono::steady_clock::time_point request_started;
  int status = 0;
  std::string method = "-";
  std::string path = "-";
};

}
