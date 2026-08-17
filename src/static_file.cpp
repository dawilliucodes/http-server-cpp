#include "static_file.h"

#include <cctype>
#include <optional>
#include <string_view>
#include <system_error>
#include <unordered_map>

namespace httpserver {

namespace {

namespace fs = std::filesystem;

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
    if (i + 2 >= in.size()) return std::nullopt;  // truncated escape
    int hi = hex_val(in[i + 1]);
    int lo = hex_val(in[i + 2]);
    if (hi < 0 || lo < 0) return std::nullopt;
    char decoded = static_cast<char>((hi << 4) | lo);
    if (decoded == '\0') return std::nullopt;  // %00 truncates C-string paths
    out.push_back(decoded);
    i += 2;
  }
  return out;
}

bool is_subpath(const fs::path& base, const fs::path& target) {
  auto b = base.begin();
  auto t = target.begin();
  for (; b != base.end(); ++b, ++t) {
    if (t == target.end() || *t != *b) return false;
  }
  return true;
}

}

PathResult resolve_path(const std::string& raw_path, const fs::path& doc_root, fs::path& out_file) {
  std::string path = raw_path.substr(0, raw_path.find('?'));

  auto decoded = url_decode(path);
  if (!decoded) return PathResult::kBadRequest;
  path = *decoded;

  if (path.empty() || path[0] != '/') return PathResult::kBadRequest;
  if (path == "/") path = "/index.html";

  const fs::path requested = fs::weakly_canonical(doc_root / path.substr(1));
  if (!is_subpath(doc_root, requested)) return PathResult::kForbidden;

  std::error_code ec;
  if (!fs::is_regular_file(requested, ec) || ec) return PathResult::kNotFound;

  out_file = requested;
  return PathResult::kOk;
}

std::string_view content_type_for(const fs::path& file) {
  std::string ext = file.extension().string();
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
