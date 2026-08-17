#pragma once

// shared by the black-box tests: real sockets only, nothing from src/.

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace test {

inline int g_total = 0;
inline int g_failed = 0;

inline void check(const std::string& name, bool ok, const std::string& detail = "") {
  ++g_total;
  if (!ok) ++g_failed;
  std::cout << (ok ? "PASS  " : "FAIL  ") << name;
  if (!detail.empty()) std::cout << "  [" << detail << "]";
  std::cout << '\n';
}

inline int summary() {
  std::cout << '\n' << (g_total - g_failed) << "/" << g_total << " passed\n";
  return g_failed == 0 ? 0 : 1;
}

inline std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

inline int connect_to(uint16_t port = 8080, int timeout_seconds = 5) {
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

  timeval tv{timeout_seconds, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  return fd;
}

// MSG_NOSIGNAL: a close mid-send must not kill the test process
inline bool send_all(int fd, std::string_view data) {
  size_t sent = 0;
  while (sent < data.size()) {
    ssize_t n = send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
    if (n <= 0) return false;
    sent += static_cast<size_t>(n);
  }
  return true;
}

inline std::string read_until_close(int fd) {
  std::string out;
  char buf[65536];
  while (true) {
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) break;
    out.append(buf, static_cast<size_t>(n));
  }
  return out;
}

// one response, framed by Content-Length or chunked, leaving the connection
// usable. empty means it closed before a complete response arrived.
inline std::string read_one_response(int fd) {
  std::string data;
  char buf[65536];

  while (data.find("\r\n\r\n") == std::string::npos) {
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) return "";
    data.append(buf, static_cast<size_t>(n));
  }

  const size_t body_start = data.find("\r\n\r\n") + 4;
  const std::string head = lower(data.substr(0, body_start));

  if (head.find("transfer-encoding: chunked") != std::string::npos) {
    while (data.size() < body_start + 5 ||
           data.compare(data.size() - 5, 5, "0\r\n\r\n") != 0) {
      ssize_t n = recv(fd, buf, sizeof(buf), 0);
      if (n <= 0) return "";
      data.append(buf, static_cast<size_t>(n));
    }
    return data;
  }

  size_t length = 0;
  if (size_t at = head.find("content-length:"); at != std::string::npos) {
    length = std::strtoul(head.c_str() + at + 15, nullptr, 10);
  }
  while (data.size() < body_start + length) {
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) return "";
    data.append(buf, static_cast<size_t>(n));
  }
  return data;
}

inline int status_of(const std::string& response) {
  if (response.rfind("HTTP/1.", 0) != 0) return 0;
  size_t space = response.find(' ');
  if (space == std::string::npos) return 0;
  return std::atoi(response.c_str() + space + 1);
}

inline std::string headers_of(const std::string& response) {
  size_t end = response.find("\r\n\r\n");
  return end == std::string::npos ? response : response.substr(0, end);
}

inline std::string body_of(const std::string& response) {
  size_t end = response.find("\r\n\r\n");
  return end == std::string::npos ? "" : response.substr(end + 4);
}

inline std::string decode_chunked(const std::string& body) {
  std::string out;
  size_t i = 0;
  while (i < body.size()) {
    size_t eol = body.find("\r\n", i);
    if (eol == std::string::npos) break;
    size_t size = std::strtoul(body.substr(i, eol - i).c_str(), nullptr, 16);
    if (size == 0) break;
    if (eol + 2 + size > body.size()) break;
    out.append(body, eol + 2, size);
    i = eol + 2 + size + 2;
  }
  return out;
}

}
