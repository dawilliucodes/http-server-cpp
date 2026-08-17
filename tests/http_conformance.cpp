#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "test_util.h"

using namespace test;

namespace {

const std::string kGet = "GET /index.html HTTP/1.1\r\nHost: x\r\n\r\n";

std::string one_shot(const std::string& request, int timeout_seconds = 5) {
  int fd = connect_to(8080, timeout_seconds);
  if (fd < 0) return "";
  send_all(fd, request);
  std::string response = read_until_close(fd);
  close(fd);
  return response;
}

void check_status(const std::string& name, const std::string& request, int want) {
  const int got = status_of(one_shot(request));
  check(name + " -> " + std::to_string(want), got == want, "got " + std::to_string(got));
}

void test_keep_alive() {
  int fd = connect_to();
  send_all(fd, kGet);
  const std::string first = read_one_response(fd);
  send_all(fd, kGet);
  const std::string second = read_one_response(fd);
  close(fd);

  check("keep-alive: 2 requests on 1 connection",
        status_of(first) == 200 && status_of(second) == 200,
        std::to_string(status_of(first)) + ", " + std::to_string(status_of(second)));
  check("keep-alive: Connection: keep-alive echoed",
        lower(headers_of(first)).find("connection: keep-alive") != std::string::npos);

  const std::string closed =
      one_shot("GET /index.html HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
  check("Connection: close honoured",
        status_of(closed) == 200 &&
            lower(headers_of(closed)).find("connection: close") != std::string::npos);

  fd = connect_to();
  send_all(fd, kGet + kGet);
  const std::string p1 = read_one_response(fd);
  const std::string p2 = read_one_response(fd);
  close(fd);
  check("pipelined requests both answered", status_of(p1) == 200 && status_of(p2) == 200,
        std::to_string(status_of(p1)) + ", " + std::to_string(status_of(p2)));

  const std::string http10 = one_shot("GET /index.html HTTP/1.0\r\nHost: x\r\n\r\n");
  check("HTTP/1.0 closes by default",
        status_of(http10) == 200 &&
            lower(headers_of(http10)).find("connection: close") != std::string::npos);
}

void test_chunked() {
  std::ifstream file("www/big.txt", std::ios::binary);
  if (!file) {
    check("chunked: www/big.txt fixture present", false, "missing - run from the repo root");
    return;
  }
  std::ostringstream on_disk;
  on_disk << file.rdbuf();

  int fd = connect_to(8080, 10);
  send_all(fd, "GET /big.txt HTTP/1.1\r\nHost: x\r\n\r\n");
  const std::string response = read_one_response(fd);
  close(fd);

  const bool chunked =
      lower(headers_of(response)).find("transfer-encoding: chunked") != std::string::npos;
  check("chunked encoding used for large file", chunked && status_of(response) == 200,
        "status=" + std::to_string(status_of(response)) +
            " chunked=" + (chunked ? "true" : "false"));
  check("chunked body terminated with 0\\r\\n\\r\\n",
        response.size() >= 5 && response.compare(response.size() - 5, 5, "0\r\n\r\n") == 0);

  const std::string decoded = decode_chunked(body_of(response));
  check("chunked body decodes to the exact file", decoded == on_disk.str(),
        std::to_string(decoded.size()) + " vs " + std::to_string(on_disk.str().size()) + " bytes");

  int fd10 = connect_to(8080, 10);
  send_all(fd10, "GET /big.txt HTTP/1.0\r\nHost: x\r\n\r\n");
  const std::string head10 = lower(headers_of(read_until_close(fd10)));
  close(fd10);
  check("HTTP/1.0 large file uses Content-Length, not chunked",
        head10.find("content-length:") != std::string::npos &&
            head10.find("chunked") == std::string::npos);
}

void test_status_codes() {
  check_status("200 index", kGet, 200);
  check_status("400 malformed", "GARBAGE\r\n\r\n", 400);
  check_status("403 traversal", "GET /../../etc/passwd HTTP/1.1\r\nHost: x\r\n\r\n", 403);
  check_status("404 missing", "GET /nope.html HTTP/1.1\r\nHost: x\r\n\r\n", 404);
  check_status("405 POST", "POST /index.html HTTP/1.1\r\nHost: x\r\n\r\n", 405);
  check_status("501 body present",
               "GET /index.html HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello", 501);
}

void test_request_size_cap() {
  check_status("413 oversized (terminator present)",
               "GET / HTTP/1.1\r\nX-Pad: " + std::string(9000, 'b') + "\r\n\r\n", 413);

  // the server answers and closes mid-send, so send_all failing here is expected
  int fd = connect_to();
  send_all(fd, "GET / HTTP/1.1\r\n");
  for (int i = 0; i < 12; ++i) {
    if (!send_all(fd, "X-Pad: " + std::string(1000, 'c') + "\r\n")) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  const int dribbled = status_of(read_until_close(fd));
  close(fd);
  check("413 oversized (no terminator) -> 413", dribbled == 413, "got " + std::to_string(dribbled));

  int under = connect_to();
  send_all(under, "GET /index.html HTTP/1.1\r\nHost: x\r\nX-Pad: " + std::string(7000, 'd') +
                      "\r\n\r\n");
  const int ok = status_of(read_one_response(under));
  close(under);
  check("just under cap still served", ok == 200, "got " + std::to_string(ok));
}

void test_exception_becomes_500() {
  // over PATH_MAX but under the header cap: weakly_canonical throws in the handler
  const int thrown =
      status_of(one_shot("GET /" + std::string(5000, 'a') + " HTTP/1.1\r\nHost: x\r\n\r\n"));
  check("handler exception -> 500", thrown == 500, "got " + std::to_string(thrown));

  int fd = connect_to();
  send_all(fd, kGet);
  const int after = status_of(read_one_response(fd));
  close(fd);
  check("server alive after handler exception", after == 200, "got " + std::to_string(after));
}

void test_idle_timeout() {
  using clock = std::chrono::steady_clock;

  int fd = connect_to(8080, 30);
  send_all(fd, "GET /index.html HTTP/1.1\r\nHost: x\r\n");  // no blank line
  auto started = clock::now();
  const int status = status_of(read_until_close(fd));
  auto waited = std::chrono::duration_cast<std::chrono::seconds>(clock::now() - started).count();
  close(fd);
  check("408 on half-sent request after idle timeout", status == 408,
        "got " + std::to_string(status) + " after " + std::to_string(waited) + "s");

  int quiet = connect_to(8080, 30);
  send_all(quiet, kGet);
  const std::string served = read_one_response(quiet);
  const std::string trailing = read_until_close(quiet);
  close(quiet);
  check("idle keep-alive connection closed by server",
        status_of(served) == 200 && trailing.empty(),
        "trailing bytes=" + std::to_string(trailing.size()));
}

}

int main() {
  if (int probe = connect_to(); probe < 0) {
    std::cerr << "no server on 127.0.0.1:8080 - start one first\n";
    return 2;
  } else {
    close(probe);
  }

  test_keep_alive();
  test_chunked();
  test_status_codes();
  test_request_size_cap();
  test_exception_becomes_500();
  test_idle_timeout();
  return summary();
}
