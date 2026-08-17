cmake_minimum_required(VERSION 3.20)
project(http-server)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED on)

# Default to a Debug build (with sanitizers) unless told otherwise.
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Release: optimized, sanitizers off (used for benchmarking).
set(CMAKE_CXX_FLAGS_RELEASE "-O2")

add_executable(server
  src/main.cpp
  src/config.cpp
  src/logging.cpp
  src/http_parse.cpp
  src/static_file.cpp
  src/http_response.cpp
  src/request_handler.cpp
  src/worker.cpp
)
target_compile_options(server PRIVATE -Wall -Wextra -Wpedantic)

# Sanitizers only in Debug builds.
target_compile_options(server PRIVATE
  $<$<CONFIG:Debug>:-g -fsanitize=address,undefined>)
target_link_options(server PRIVATE
  $<$<CONFIG:Debug>:-fsanitize=address,undefined>)

# Tsan: ThreadSanitizer, checked separately since it can't be combined with
# ASan in the same binary.
target_compile_options(server PRIVATE
  $<$<CONFIG:Tsan>:-g -fsanitize=thread>)
target_link_options(server PRIVATE
  $<$<CONFIG:Tsan>:-fsanitize=thread>)

# Generated, not committed: the conformance test needs a file bigger than the
# server's 64KB read block to get a chunked response out of it.
set(big_fixture ${CMAKE_CURRENT_SOURCE_DIR}/www/big.txt)
if(NOT EXISTS ${big_fixture})
  string(REPEAT "the quick brown fox jumps over the lazy dog\n" 6000 big_contents)
  file(WRITE ${big_fixture} "${big_contents}")
endif()

# Black-box tests: they drive the built binary over real sockets and don't link
# against src/, so they're built plain - no sanitizers, whatever the config.
find_package(Threads REQUIRED)
foreach(test_name http_conformance graceful_shutdown)
  add_executable(test_${test_name} tests/${test_name}.cpp)
  target_compile_options(test_${test_name} PRIVATE -Wall -Wextra -Wpedantic -g)
  target_link_libraries(test_${test_name} PRIVATE Threads::Threads)
endforeach()
