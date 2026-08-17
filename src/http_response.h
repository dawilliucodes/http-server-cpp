#pragma once

#include <string>
#include <string_view>

namespace httpserver {

std::string build_response(int status_code, std::string_view reason, std::string_view content_type,
                            const std::string& body, bool keep_alive);

// errors close by default: after most of them we've lost track of where we are
// in the byte stream
std::string build_error_response(int status_code, std::string_view reason, bool keep_alive = false);

// for bodies whose length isn't known when the headers go out. HTTP/1.1 only.
std::string build_chunked_headers(int status_code, std::string_view reason,
                                   std::string_view content_type, bool keep_alive);
std::string encode_chunk(std::string_view data);  // "<hex size>\r\n<data>\r\n"
std::string final_chunk();

}
