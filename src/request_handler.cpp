#include "request_handler.h"

#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

#include "http_parse.h"
#include "http_response.h"
#include "static_file.h"

namespace httpserver {

namespace {
namespace fs = std::filesystem;

constexpr size_t kStreamBlock = 64 * 1024;

bool equals_ignoring_case(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != b[i]) return false;
  }
  return true;
}

// HTTP/1.1 holds the connection open unless the client says otherwise;
// HTTP/1.0 closes unless the client explicitly asks to keep it
bool wants_keep_alive(const RequestLine& request_line,
                      const std::unordered_map<std::string, std::string>& headers) {
  const bool http_10 = request_line.version == "HTTP/1.0";
  auto it = headers.find("connection");
  if (it == headers.end()) return !http_10;
  return http_10 ? equals_ignoring_case(it->second, "keep-alive")
                 : !equals_ignoring_case(it->second, "close");
}
}

Response handle_request(std::string_view header_text, const fs::path& doc_root,
                        bool allow_keep_alive) {
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

  // we don't consume bodies yet, so one would sit in the buffer and be misread
  // as the next request - refuse rather than desync the stream
  if (headers.count("content-length") || headers.count("transfer-encoding")) {
    return error(501, "Not Implemented");
  }

  const bool keep_alive = allow_keep_alive && wants_keep_alive(*request_line, headers);

  if (request_line->method != "GET") return error(405, "Method Not Allowed", keep_alive);

  fs::path file_path;
  switch (resolve_path(request_line->path, doc_root, file_path)) {
    case PathResult::kBadRequest:
      return error(400, "Bad Request");
    case PathResult::kForbidden:
      return error(403, "Forbidden", keep_alive);
    case PathResult::kNotFound:
      return error(404, "Not Found", keep_alive);
    case PathResult::kOk:
      break;
  }

  std::ifstream file(file_path, std::ios::binary);
  if (!file) return error(404, "Not Found", keep_alive);

  const std::string_view content_type = content_type_for(file_path);

  // read one block first: if it held the whole file we know the length and can
  // send Content-Length, otherwise we don't and have to chunk
  std::string body(kStreamBlock, '\0');
  file.read(body.data(), static_cast<std::streamsize>(body.size()));
  body.resize(static_cast<size_t>(file.gcount()));
  const bool whole_file_read = !file || file.eof();

  if (whole_file_read || request_line->version == "HTTP/1.0") {
    if (!whole_file_read) {
      std::ostringstream rest;
      rest << file.rdbuf();
      body += rest.str();
    }
    return respond(build_response(200, "OK", content_type, body, keep_alive), keep_alive, 200);
  }

  std::string out = build_chunked_headers(200, "OK", content_type, keep_alive);
  out += encode_chunk(body);
  while (true) {
    std::string next(kStreamBlock, '\0');
    file.read(next.data(), static_cast<std::streamsize>(next.size()));
    next.resize(static_cast<size_t>(file.gcount()));
    if (next.empty()) break;
    out += encode_chunk(next);
  }
  out += final_chunk();
  return respond(std::move(out), keep_alive, 200);
}

}
