#include "config.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include <getopt.h>

namespace httpserver {

namespace {

bool parse_number(const char* text, unsigned long min, unsigned long max, unsigned long& out) {
  if (text == nullptr || *text == '\0' || *text == '-') return false;

  errno = 0;
  char* end = nullptr;
  unsigned long value = std::strtoul(text, &end, 10);
  if (errno == ERANGE || end == text || *end != '\0' || value < min || value > max) return false;

  out = value;
  return true;
}

}

void print_usage(const char* program) {
  std::cout
      << "Usage: " << program << " [options]\n"
      << "\n"
      << "A static file server: one epoll event loop per core, HTTP/1.1 keep-alive.\n"
      << "\n"
      << "Options:\n"
      << "  -p, --port PORT        port to listen on (default 8080)\n"
      << "  -d, --docroot DIR      directory to serve files from (default www)\n"
      << "  -w, --workers N        event-loop threads (default: one per core)\n"
      << "  -l, --log PATH         access log file (default: stdout)\n"
      << "  -h, --help             show this message and exit\n"
      << "\n"
      << "Log lines are: timestamp, client IP, request, status, bytes, microseconds.\n";
}

std::optional<Config> parse_args(int argc, char** argv, int& exit_code) {
  static const option long_options[] = {
      {"port", required_argument, nullptr, 'p'},
      {"docroot", required_argument, nullptr, 'd'},
      {"workers", required_argument, nullptr, 'w'},
      {"log", required_argument, nullptr, 'l'},
      {"help", no_argument, nullptr, 'h'},
      {nullptr, 0, nullptr, 0},
  };

  Config config;
  auto reject = [&exit_code](const char* what, const char* expected) {
    std::cerr << "invalid " << what << ": " << optarg << " (expected " << expected << ")\n";
    exit_code = 2;
    return std::optional<Config>{};
  };

  int opt;
  while ((opt = getopt_long(argc, argv, "p:d:w:l:h", long_options, nullptr)) != -1) {
    unsigned long value = 0;
    switch (opt) {
      case 'p':  // 0 would have the kernel pick a port we never report
        if (!parse_number(optarg, 1, 65535, value)) return reject("port", "1-65535");
        config.port = static_cast<uint16_t>(value);
        break;

      case 'w':
        if (!parse_number(optarg, 1, 1024, value)) return reject("worker count", "1-1024");
        config.workers = static_cast<unsigned int>(value);
        break;

      case 'd':
        config.doc_root = optarg;
        break;

      case 'l':
        config.log_path = optarg;
        break;

      case 'h':
        print_usage(argv[0]);
        exit_code = 0;
        return std::nullopt;

      default:  // getopt_long already said what was wrong
        std::cerr << "try '" << argv[0] << " --help'\n";
        exit_code = 2;
        return std::nullopt;
    }
  }

  if (optind < argc) {
    std::cerr << "unexpected argument: " << argv[optind] << "\n"
              << "try '" << argv[0] << " --help'\n";
    exit_code = 2;
    return std::nullopt;
  }

  return config;
}

}
