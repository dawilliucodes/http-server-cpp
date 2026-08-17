#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace httpserver {

bool fail(const char* what);  // logs errno for `what`, always returns false
void log_line(const std::string& message);

struct AccessRecord {
  std::string_view client_ip;
  std::string_view method;  // "-" if the request didn't parse
  std::string_view path;
  int status = 0;
  size_t bytes_sent = 0;  // whole response, headers included
  long long micros = 0;   // request parsed -> last byte written
};

bool open_access_log(const std::string& path);  // empty path = stdout
void log_access(const AccessRecord& record);
void close_access_log();

}
