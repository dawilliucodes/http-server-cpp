#include "http_response.h"

#include <cstdio>

namespace httpserver {

namespace {

void append_head(std::string& out, int status_code, std::string_view reason,
                 std::string_view content_type) {
  out += "HTTP/1.1 ";
  out += std::to_string(status_code);
  out += ' ';
  out += reason;
  out += "\r\nContent-Type: ";
  out += content_type;
  out += "\r\n";
}

std::string_view connection(bool keep_alive) {
  return keep_alive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
}

}

std::string build_response(int status_code, std::string_view reason, std::string_view content_type,
                            std::string_view body, bool keep_alive) {
  const std::string length = std::to_string(body.size());
  const std::string_view tail = connection(keep_alive);

  std::string out;
  out.reserve(48 + reason.size() + content_type.size() + length.size() + tail.size() + body.size());
  append_head(out, status_code, reason, content_type);
  out += "Content-Length: ";
  out += length;
  out += "\r\n";
  out += tail;
  out += body;
  return out;
}

std::string build_error_response(int status_code, std::string_view reason, bool keep_alive) {
  return build_response(status_code, reason, "text/plain",
                         std::to_string(status_code) + " " + std::string(reason) + "\n", keep_alive);
}

std::string build_chunked_headers(int status_code, std::string_view reason,
                                   std::string_view content_type, bool keep_alive) {
  std::string out;
  append_head(out, status_code, reason, content_type);
  out += "Transfer-Encoding: chunked\r\n";
  out += connection(keep_alive);
  return out;
}

std::string encode_chunk(std::string_view data) {
  if (data.empty()) return "";
  char size[32];
  int len = std::snprintf(size, sizeof(size), "%zx\r\n", data.size());
  std::string out(size, static_cast<size_t>(len));
  out.append(data);
  out.append("\r\n");
  return out;
}

std::string final_chunk() { return "0\r\n\r\n"; }

}
