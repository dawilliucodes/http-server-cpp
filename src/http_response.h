#pragma once

#include <string>
#include <string_view>

namespace httpserver {

std::string build_response(int status_code, std::string_view reason, std::string_view content_type,
                            std::string_view body, bool keep_alive);

std::string build_error_response(int status_code, std::string_view reason, bool keep_alive = false);

std::string build_chunked_headers(int status_code, std::string_view reason,
                                   std::string_view content_type, bool keep_alive);
std::string encode_chunk(std::string_view data);
std::string final_chunk();

}
