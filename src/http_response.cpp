#include "http_response.h"

#include <cstdio>

namespace httpserver {

namespace {

std::string head(int status_code, std::string_view reason, std::string_view content_type) {
  return "HTTP/1.1 " + std::to_string(status_code) + " " + std::string(reason) +
         "\r\nContent-Type: " + std::string(content_type) + "\r\n";
}

std::string_view connection(bool keep_alive) {
  return keep_alive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
}

}

std::string build_response(int status_code, std::string_view reason, std::string_view content_type,
                            const std::string& body, bool keep_alive) {
  return head(status_code, reason, content_type) + "Content-Length: " +
         std::to_string(body.size()) + "\r\n" + std::string(connection(keep_alive)) + body;
}

std::string build_error_response(int status_code, std::string_view reason, bool keep_alive) {
  return build_response(status_code, reason, "text/plain",
                         std::to_string(status_code) + " " + std::string(reason) + "\n", keep_alive);
}

std::string build_chunked_headers(int status_code, std::string_view reason,
                                   std::string_view content_type, bool keep_alive) {
  return head(status_code, reason, content_type) + "Transfer-Encoding: chunked\r\n" +
         std::string(connection(keep_alive));
}

std::string encode_chunk(std::string_view data) {
  if (data.empty()) return "";  // a zero-length chunk marks the end of the body
  char size[32];
  int len = std::snprintf(size, sizeof(size), "%zx\r\n", data.size());
  std::string out(size, static_cast<size_t>(len));
  out.append(data);
  out.append("\r\n");
  return out;
}

std::string final_chunk() { return "0\r\n\r\n"; }

}
