# http-server

A static HTTP/1.1 file server for Linux, written from scratch in C++20 on
`epoll` — one non-blocking event loop per core, no external dependencies.

## Features

- **One event loop per core.** `hardware_concurrency()` worker threads, each
  owning its own `epoll` instance. A connection belongs to a single worker for
  its whole life, so nothing on the request path is shared or locked.
- **HTTP/1.1 keep-alive**, including pipelined requests: bytes left over after
  one request start the next, and are handled without waiting for another read.
  `Connection:` is answered to match, and HTTP/1.0 closes unless it asks not to.
- **Chunked transfer encoding** for files whose length isn't known when the
  headers go out, with a buffered `Content-Length` fallback for HTTP/1.0.
- **Idle timeouts.** A connection that goes quiet mid-request gets a 408; one
  idle between requests is dropped. Both are swept from a single per-worker
  `timerfd` rather than a timer per connection.
- **Graceful shutdown** on `SIGTERM`/`SIGINT` via `signalfd`: stops accepting,
  answers everything already in flight with `Connection: close`, and exits
  without resetting or dropping a connection.
- **Path-traversal protection.** Request paths are percent-decoded and
  canonicalized before being checked against the document root, so `../` and
  encoded variants like `%2e%2e` are both rejected.
- **Access logging** with client IP, request line, status, bytes and handling
  time, serialized so lines from different workers can't interleave.
- **Faults are contained.** An exception anywhere in request handling becomes a
  500 for that connection instead of taking down the worker serving every other
  connection on it. Every syscall is checked.
- Status codes: 200, 400, 403, 404, 405, 408, 413, 500, 501.

## Architecture

```
                ┌───────────────┐
   clients ────▶│  accept loop  │   main thread: accept() + signalfd, nothing else
                └───────┬───────┘
                        │  round-robin hand-off, woken by eventfd
          ┌─────────────┼─────────────┐
          ▼             ▼             ▼
     ┌─────────┐   ┌─────────┐   ┌─────────┐
     │ worker  │   │ worker  │   │ worker  │   epoll loop + thread each,
     │ epoll   │   │ epoll   │   │ epoll   │   owning its connections outright
     └─────────┘   └─────────┘   └─────────┘
```

The main thread does nothing but accept connections and hand each to the next
worker over that worker's `eventfd`. From then on the connection is that
worker's alone — its read buffer, write buffer, write offset and stage all live
in state only that thread touches, so the hot path needs no synchronization at
all. The only shared state in the process is the access log, behind a mutex.

Each connection is a small state machine driven by readiness events: read until
the end of the headers, build a response, switch the socket to write interest,
write from a stored offset until the buffer drains, then either close or go back
to reading for the next request on the same connection.

## Requirements

- Linux (or WSL2) — uses `epoll`, `eventfd`, `timerfd` and `signalfd`
- A C++20 compiler (developed against g++ 13)
- CMake ≥ 3.20

## Building

```bash
# Debug — AddressSanitizer + UndefinedBehaviorSanitizer (default)
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug

# Release — -O2, sanitizers off
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release

# Tsan — ThreadSanitizer, kept separate since it can't be linked alongside ASan
cmake -S . -B build/tsan -DCMAKE_BUILD_TYPE=Tsan
cmake --build build/tsan
```

> On WSL2, ThreadSanitizer's fixed shadow-memory layout can collide with a PIE
> binary's randomized load address and abort with `FATAL: ThreadSanitizer:
> unexpected memory mapping`. Disable ASLR for that run:
> `setarch "$(uname -m)" -R ./build/tsan/server`.

## Running

```bash
./build/release/server
# listening on http://localhost:8080 (doc root: "/home/you/http-server/www", 12 event-loop workers, log: stdout)
```

```bash
curl http://localhost:8080/       # serves index.html from the document root
curl -v http://localhost:8080/    # with response headers
```

## Configuration

```
Usage: server [options]

Options:
  -p, --port PORT        port to listen on (default 8080)
  -d, --docroot DIR      directory to serve files from (default www)
  -w, --workers N        event-loop threads (default: one per core)
  -l, --log PATH         access log file (default: stdout)
  -h, --help             show this message and exit
```

```bash
./build/release/server --port 8000 --docroot /srv/www --workers 4 --log /var/log/http-server.log
```

Invalid values are rejected at startup rather than silently defaulted, and a
document root that isn't a directory fails immediately instead of
404-ing every request. The startup banner and errors go to stderr, leaving
stdout for the access log.

### Access log

One line per completed request, appended to the log file or written to stdout:

```
2026-08-17T15:41:01.449629Z 127.0.0.1 "GET /big.txt" 200 252137 3403us
│                           │         │              │   │      └─ handling time
│                           │         │              │   └─ bytes sent, headers included
│                           │         │              └─ status
│                           │         └─ request line
│                           └─ client
└─ UTC, microsecond precision
```

Timestamps are UTC so lines sort lexicographically. The duration covers the
point a complete request is parsed to the last byte written — server handling
time, not however long the client took to send. A request too malformed to yield
a request line logs `"- -"` rather than inventing a method and path.

Each line is assembled before the lock is taken and written in one call, so
concurrent workers can't interleave, and every line is flushed — `tail -f` shows
requests as they complete.

## Testing

Two black-box test binaries drive the server over real sockets. They're built by
the same CMake project but link nothing from `src/`; they only speak HTTP, so
they exercise the server exactly as a client sees it. Run from the repo root.

```bash
# Conformance: keep-alive, pipelining, chunked encoding, idle timeouts and every
# status code. Needs a server already running. Takes ~25s, since two of the
# checks wait out the 10s idle timeout.
./build/debug/server &
./build/debug/test_http_conformance

# Graceful shutdown: signals the server mid-load and checks nothing is reset or
# truncated. Starts and stops the server itself.
./build/debug/test_graceful_shutdown ./build/debug/server SIGTERM
./build/debug/test_graceful_shutdown ./build/debug/server SIGINT
```

Both pass against Debug, Release and Tsan builds. The shutdown test takes a
command rather than a bare path, so the ThreadSanitizer build can be wrapped:

```bash
./build/debug/test_graceful_shutdown 'setarch $(uname -m) -R ./build/tsan/server' SIGTERM
```

They cover what `curl` can't express: stopping a request half-sent, pipelining
two requests into a single `write()`, decoding a chunked body byte-for-byte
against the file on disk, holding a connection open past the idle timeout, and
signalling the server while 40 concurrent keep-alive clients are mid-flight.

Quick manual checks:

```bash
curl -v http://localhost:8080/index.html                                        # 200, keep-alive
curl -o /dev/null -w '%{http_code}\n' http://localhost:8080/missing.html        # 404
curl -o /dev/null -w '%{http_code}\n' -X POST http://localhost:8080/index.html  # 405
curl -o /dev/null -w '%{http_code}\n' --path-as-is http://localhost:8080/../../etc/passwd  # 403

curl -sD- -o /dev/null http://localhost:8080/big.txt | grep -i transfer-encoding # chunked
ls /proc/$(pgrep -x server)/task | wc -l                                         # threads stay fixed
```

## Project layout

```
http-server/
├── CMakeLists.txt
├── src/
│   ├── main.cpp                 # listener, signalfd, accept loop, worker fan-out
│   ├── config.{h,cpp}           # CLI parsing and --help
│   ├── logging.{h,cpp}          # access log and stderr diagnostics
│   ├── worker.{h,cpp}           # epoll loop and per-connection state machine
│   ├── connection_state.h       # per-connection buffers, stage, timestamps
│   ├── request_handler.{h,cpp}  # parsed request in, response out
│   ├── http_parse.{h,cpp}       # request-line and header parsing
│   ├── http_response.{h,cpp}    # response and chunk formatting
│   └── static_file.{h,cpp}      # path resolution, traversal checks, MIME types
├── tests/
│   ├── test_util.h              # socket and response-framing helpers
│   ├── http_conformance.cpp
│   └── graceful_shutdown.cpp
└── www/                         # default document root
```

`www/big.txt` is generated by CMake rather than checked in: the conformance test
needs a file larger than the server's 64KB read block to get a chunked response
out of it.

`http_parse`, `http_response`, `static_file` and `request_handler` are pure —
they take bytes and return bytes, with no sockets involved. All I/O lives in
`worker` and `main`, which keeps the protocol logic testable and the event loop
free of parsing.
