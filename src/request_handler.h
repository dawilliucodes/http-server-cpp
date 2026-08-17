#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace httpserver {

struct Response {
  std::string text;
  bool keep_alive = false;
  int status = 0;  // status/method/path are for the access log
  std::string method = "-";
  std::string path = "-";
};

// header_text runs to the last header's \r\n, not the blank line after it.
// allow_keep_alive vetoes reuse whatever the client asked for.
Response handle_request(std::string_view header_text, const std::filesystem::path& doc_root,
                        bool allow_keep_alive = true);

}
