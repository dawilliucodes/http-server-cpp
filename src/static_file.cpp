#include "static_file.h"

#include <cctype>
#include <cerrno>
#include <optional>
#include <unordered_map>

#include <fcntl.h>
#include <linux/openat2.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace httpserver {

namespace {

std::optional<std::string> url_decode(std::string_view in) {
  std::string out;
  out.reserve(in.size());
  auto hex_val = [](char h) -> int {
    if (h >= '0' && h <= '9') return h - '0';
    if (h >= 'a' && h <= 'f') return h - 'a' + 10;
    if (h >= 'A' && h <= 'F') return h - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < in.size(); ++i) {
    char c = in[i];
    if (c != '%') {
      out.push_back(c);
      continue;
    }
    if (i + 2 >= in.size()) return std::nullopt;
    int hi = hex_val(in[i + 1]);
    int lo = hex_val(in[i + 2]);
    if (hi < 0 || lo < 0) return std::nullopt;
    char decoded = static_cast<char>((hi << 4) | lo);
    if (decoded == '\0') return std::nullopt;
    out.push_back(decoded);
    i += 2;
  }
  return out;
}

long open_beneath(int root_fd, const char* path) {
  open_how how{};
  how.flags = O_RDONLY | O_CLOEXEC;
  how.resolve = RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS;
  return syscall(SYS_openat2, root_fd, path, &how, sizeof(how));
}

}

bool resolve_beneath_supported(int root_fd) {
  long fd = open_beneath(root_fd, ".");
  if (fd >= 0) {
    close(static_cast<int>(fd));
    return true;
  }
  return errno != ENOSYS && errno != EINVAL;
}

OpenedFile open_under_root(const std::string& raw_path, int root_fd) {
  OpenedFile out;

  std::string path = raw_path.substr(0, raw_path.find('?'));
  auto decoded = url_decode(path);
  if (!decoded) {
    out.result = PathResult::kBadRequest;
    return out;
  }
  path = *decoded;

  if (path.empty() || path[0] != '/') {
    out.result = PathResult::kBadRequest;
    return out;
  }
  if (path == "/") path = "/index.html";
  out.relative = path.substr(1);

  long fd = open_beneath(root_fd, out.relative.c_str());
  if (fd < 0) {
    switch (errno) {
      case EXDEV:
      case ELOOP:
      case EACCES:
      case EPERM:
        out.result = PathResult::kForbidden;
        break;
      case ENAMETOOLONG:
        out.result = PathResult::kBadRequest;
        break;
      default:
        out.result = PathResult::kNotFound;
    }
    return out;
  }

  struct stat info {};
  if (fstat(static_cast<int>(fd), &info) < 0 || !S_ISREG(info.st_mode)) {
    close(static_cast<int>(fd));
    out.result = PathResult::kNotFound;
    return out;
  }

  out.fd = static_cast<int>(fd);
  out.result = PathResult::kOk;
  return out;
}

std::string_view content_type_for(std::string_view path) {
  const size_t slash = path.rfind('/');
  const size_t dot = path.rfind('.');
  if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) {
    return "text/plain";
  }

  std::string ext(path.substr(dot));
  for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

  static const std::unordered_map<std::string, std::string_view> kTypes = {
      {".html", "text/html"}, {".htm", "text/html"}, {".css", "text/css"},
      {".js", "application/javascript"}, {".png", "image/png"},
      {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"}, {".txt", "text/plain"},
  };
  auto it = kTypes.find(ext);
  return it != kTypes.end() ? it->second : "text/plain";
}

}
