#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

#include "test_util.h"

using namespace test;

namespace {

constexpr int kClients = 40;
constexpr int kRequestsPerConnection = 50;

std::atomic<bool> g_stop{false};
std::atomic<int> g_ok{0};
std::atomic<int> g_reset{0};      // RST rather than an orderly close
std::atomic<int> g_truncated{0};  // request sent, connection closed with no reply
std::atomic<int> g_refused{0};    // expected once the listener is gone
std::atomic<int> g_other{0};

std::mutex g_detail_mutex;
std::vector<std::string> g_details;
std::chrono::steady_clock::time_point g_signal_at;
std::atomic<bool> g_signalled{false};

void note(const std::string& text) {
  std::string when;
  if (g_signalled.load()) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - g_signal_at)
                  .count();
    when = " t+" + std::to_string(ms) + "ms";
  }
  std::lock_guard<std::mutex> lock(g_detail_mutex);
  g_details.push_back(text + when);
}

void client_loop() {
  while (!g_stop.load()) {
    int fd = connect_to(8080, 5);
    if (fd < 0) {
      if (errno == ECONNREFUSED) {
        ++g_refused;
      } else {
        ++g_other;
      }
      return;
    }

    int served = 0;
    for (int i = 0; i < kRequestsPerConnection && !g_stop.load(); ++i) {
      if (!send_all(fd, "GET /index.html HTTP/1.1\r\nHost: x\r\n\r\n")) {
        if (errno == ECONNRESET) {
          ++g_reset;
          note("reset: RST on send, req #" + std::to_string(served + 1));
        } else {
          ++g_truncated;
          note("truncated: send failed, req #" + std::to_string(served + 1));
        }
        break;
      }

      const std::string response = read_one_response(fd);
      if (response.empty()) {
        if (errno == ECONNRESET) {
          ++g_reset;
          note("reset: RST on read, req #" + std::to_string(served + 1));
        } else {
          ++g_truncated;
          note("truncated: no response, req #" + std::to_string(served + 1));
        }
        break;
      }

      ++served;
      if (status_of(response) == 200) {
        ++g_ok;
      } else {
        ++g_other;
        note("other: " + response.substr(0, response.find('\r')));
      }

      if (lower(headers_of(response)).find("connection: close") != std::string::npos) break;
    }
    close(fd);
  }
}

}

int main(int argc, char** argv) {
  const std::string command = argc > 1 ? argv[1] : "./build/debug/server";
  const std::string signal_name = argc > 2 ? argv[2] : "SIGTERM";
  if (signal_name != "SIGTERM" && signal_name != "SIGINT") {
    std::cerr << "usage: " << argv[0] << " <server-command> [SIGTERM|SIGINT]\n";
    return 2;
  }
  const int signal_number = signal_name == "SIGINT" ? SIGINT : SIGTERM;

  int pipe_fds[2];
  if (pipe(pipe_fds) < 0) {
    std::cerr << "pipe failed\n";
    return 2;
  }

  pid_t pid = fork();
  if (pid < 0) {
    std::cerr << "fork failed\n";
    return 2;
  }
  if (pid == 0) {
    close(pipe_fds[0]);
    dup2(pipe_fds[1], STDOUT_FILENO);
    dup2(pipe_fds[1], STDERR_FILENO);
    close(pipe_fds[1]);
    // exec so the signal reaches the server, not an intervening shell
    const std::string wrapped = "exec " + command;
    execl("/bin/sh", "sh", "-c", wrapped.c_str(), nullptr);
    _exit(127);
  }
  close(pipe_fds[1]);

  std::this_thread::sleep_for(std::chrono::milliseconds(800));

  std::vector<std::thread> clients;
  clients.reserve(kClients);
  for (int i = 0; i < kClients; ++i) clients.emplace_back(client_loop);

  std::this_thread::sleep_for(std::chrono::seconds(2));

  std::cout << "sending " << signal_name << " mid-load...\n";
  g_signal_at = std::chrono::steady_clock::now();
  g_signalled.store(true);
  kill(pid, signal_number);

  int status = 0;
  waitpid(pid, &status, 0);
  const auto shutdown_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - g_signal_at)
                               .count();

  g_stop.store(true);
  for (auto& client : clients) client.join();

  std::string output;
  char buf[4096];
  ssize_t n;
  while ((n = read(pipe_fds[0], buf, sizeof(buf))) > 0) output.append(buf, static_cast<size_t>(n));
  close(pipe_fds[0]);

  const int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  std::cout << "exit code: " << exit_code << "   shutdown took " << shutdown_ms << "ms\n";
  std::cout << "--- server output ---\n" << output;
  std::cout << "--- client results ---\n"
            << "ok=" << g_ok.load() << " reset=" << g_reset.load()
            << " truncated=" << g_truncated.load() << " refused=" << g_refused.load()
            << " other=" << g_other.load() << '\n';
  for (const auto& detail : g_details) std::cout << "    " << detail << '\n';

  check("exit code is 0", exit_code == 0, "got " + std::to_string(exit_code));
  check("no connection resets", g_reset.load() == 0, std::to_string(g_reset.load()) + " resets");
  check("no truncated responses", g_truncated.load() == 0,
        std::to_string(g_truncated.load()) + " truncated");
  check("load actually got going", g_ok.load() > 0, std::to_string(g_ok.load()) + " served");
  check("server reported a clean shutdown",
        output.find("shutdown complete") != std::string::npos);
  check("shutdown was prompt", shutdown_ms < 15000, std::to_string(shutdown_ms) + "ms");
  return summary();
}
