#include "request_handler.h"

#include <cctype>
#include <string>
#include <unordered_map>

#include <unistd.h>

#include "http_parse.h"
#include "http_response.h"
#include "static_file.h"

namespace httpserver {

namespace {

constexpr size_t kStreamBlock = 64 * 1024;

class FdGuard {
 public:
  explicit FdGuard(int fd) : fd_(fd) {}
  ~FdGuard() {
    if (fd_ >= 0) close(fd_);
  }
  FdGuard(const FdGuard&) = delete;
  FdGuard& operator=(const FdGuard&) = delete;

 private:
  int fd_;
};

size_t read_block(int fd, std::string& into) {
  size_t total = 0;
  while (total < into.size()) {
    ssize_t n = read(fd, into.data() + total, into.size() - total);
    if (n <= 0) break;
    total += static_cast<size_t>(n);
  }
  return total;
}

bool equals_ignoring_case(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != b[i]) return false;
  }
  return true;
}

bool wants_keep_alive(const RequestLine& request_line,
                      const std::unordered_map<std::string, std::string>& headers) {
  const bool http_10 = request_line.version == "HTTP/1.0";
  auto it = headers.find("connection");
  if (it == headers.end()) return !http_10;
  return http_10 ? equals_ignoring_case(it->second, "keep-alive")
                 : !equals_ignoring_case(it->second, "close");
}

}

Response handle_request(std::string_view header_text, int doc_root_fd, bool allow_keep_alive) {
  size_t line_end = header_text.find("\r\n");

  auto request_line = parse_request_line(header_text.substr(0, line_end));

  auto respond = [&request_line](std::string text, bool keep_alive, int status) {
    Response response;
    response.text = std::move(text);
    response.keep_alive = keep_alive;
    response.status = status;
    if (request_line) {
      response.method = request_line->method;
      response.path = request_line->path;
    }
    return response;
  };
  auto error = [&respond](int status, std::string_view reason, bool keep_alive = false) {
    return respond(build_error_response(status, reason, keep_alive), keep_alive, status);
  };

  if (!request_line) return error(400, "Bad Request");

  std::unordered_map<std::string, std::string> headers;
  if (!parse_headers(header_text.substr(line_end + 2), headers)) return error(400, "Bad Request");

  if (headers.count("content-length") || headers.count("transfer-encoding")) {
    return error(501, "Not Implemented");
  }

  const bool keep_alive = allow_keep_alive && wants_keep_alive(*request_line, headers);

  if (request_line->method != "GET") return error(405, "Method Not Allowed", keep_alive);

  OpenedFile file = open_under_root(request_line->path, doc_root_fd);
  switch (file.result) {
    case PathResult::kBadRequest:
      return error(400, "Bad Request");
    case PathResult::kForbidden:
      return error(403, "Forbidden", keep_alive);
    case PathResult::kNotFound:
      return error(404, "Not Found", keep_alive);
    case PathResult::kOk:
      break;
  }
  FdGuard guard(file.fd);

  const std::string_view content_type = content_type_for(file.relative);

  std::string body(kStreamBlock, '\0');
  body.resize(read_block(file.fd, body));
  const bool whole_file_read = body.size() < kStreamBlock;

  if (whole_file_read || request_line->version == "HTTP/1.0") {
    while (!whole_file_read) {
      std::string next(kStreamBlock, '\0');
      next.resize(read_block(file.fd, next));
      if (next.empty()) break;
      body += next;
    }
    return respond(build_response(200, "OK", content_type, body, keep_alive), keep_alive, 200);
  }

  std::string out = build_chunked_headers(200, "OK", content_type, keep_alive);
  out += encode_chunk(body);
  while (true) {
    std::string next(kStreamBlock, '\0');
    next.resize(read_block(file.fd, next));
    if (next.empty()) break;
    out += encode_chunk(next);
  }
  out += final_chunk();
  return respond(std::move(out), keep_alive, 200);
}

}
