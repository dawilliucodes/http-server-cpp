#pragma once

#include <string>
#include <string_view>

namespace httpserver {

struct Response {
  std::string text;
  bool keep_alive = false;
  int status = 0;
  std::string method = "-";
  std::string path = "-";
};

// header_text runs to the last header's \r\n, not the blank line after it
Response handle_request(std::string_view header_text, int doc_root_fd,
                        bool allow_keep_alive = true);

}
