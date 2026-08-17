#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace httpserver {

enum class PathResult { kOk, kBadRequest, kForbidden, kNotFound };

// doc_root must already be canonical
PathResult resolve_path(const std::string& raw_path, const std::filesystem::path& doc_root,
                         std::filesystem::path& out_file);

std::string_view content_type_for(const std::filesystem::path& file);

}
