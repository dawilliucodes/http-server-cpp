#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace httpserver {

struct Config {
  uint16_t port = 8080;
  std::filesystem::path doc_root = "www";
  unsigned int workers = 0;  // 0 = one per core
  std::string log_path;      // empty = stdout
};

// nullopt means don't start; exit_code says whether that's an error or --help
std::optional<Config> parse_args(int argc, char** argv, int& exit_code);

void print_usage(const char* program);

}
