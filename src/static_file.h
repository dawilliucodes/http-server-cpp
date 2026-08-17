#pragma once

#include <string>
#include <string_view>

namespace httpserver {

enum class PathResult { kOk, kBadRequest, kForbidden, kNotFound };

struct OpenedFile {
  PathResult result = PathResult::kNotFound;
  int fd = -1;  // caller closes
  std::string relative;
};

// the kernel refuses anything resolving outside root_fd
OpenedFile open_under_root(const std::string& raw_path, int root_fd);

bool resolve_beneath_supported(int root_fd);  // needs Linux 5.6+

std::string_view content_type_for(std::string_view path);

}
