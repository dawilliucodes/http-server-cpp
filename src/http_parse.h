#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace httpserver {

struct RequestLine {
  std::string method;
  std::string path;
  std::string version;
};

std::optional<RequestLine> parse_request_line(std::string_view line);

bool parse_headers(std::string_view block,
                   std::unordered_map<std::string, std::string>& headers);  // keys lowercased

}
