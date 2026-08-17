#include "http_parse.h"

#include <cctype>

namespace httpserver {

std::optional<RequestLine> parse_request_line(std::string_view line) {
  size_t first_space = line.find(' ');
  if (first_space == std::string_view::npos) return std::nullopt;
  size_t second_space = line.find(' ', first_space + 1);
  if (second_space == std::string_view::npos) return std::nullopt;
  if (line.find(' ', second_space + 1) != std::string_view::npos) return std::nullopt;

  RequestLine rl;
  rl.method = std::string(line.substr(0, first_space));
  rl.path = std::string(line.substr(first_space + 1, second_space - first_space - 1));
  rl.version = std::string(line.substr(second_space + 1));

  if (rl.method.empty() || rl.path.empty() || rl.version.rfind("HTTP/", 0) != 0) {
    return std::nullopt;
  }
  return rl;
}

bool parse_headers(std::string_view block, std::unordered_map<std::string, std::string>& headers) {
  size_t pos = 0;
  while (pos < block.size()) {
    size_t eol = block.find("\r\n", pos);
    std::string_view line = (eol == std::string_view::npos) ? block.substr(pos) : block.substr(pos, eol - pos);
    if (!line.empty()) {
      size_t colon = line.find(':');
      if (colon == std::string_view::npos) return false;
      std::string key(line.substr(0, colon));
      size_t value_start = colon + 1;
      while (value_start < line.size() && line[value_start] == ' ') ++value_start;
      std::string value(line.substr(value_start));
      for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      headers[std::move(key)] = std::move(value);
    }
    if (eol == std::string_view::npos) break;
    pos = eol + 2;
  }
  return true;
}

}
