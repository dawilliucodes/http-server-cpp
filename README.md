# http-server

A static HTTP/1.1 file server for Linux, written from scratch in C++20 on `epoll`.
One non-blocking event loop per core, no external dependencies.

## Features

- One `epoll` event loop per core. A connection is owned by one worker for life, so the request path needs no locking
- HTTP/1.1 keep-alive and request pipelining
- Chunked transfer encoding, with a `Content-Length` fallback for HTTP/1.0
- Idle timeouts from a per-worker `timerfd`: 408 mid-request, silent close between requests
- Graceful shutdown on `SIGTERM`/`SIGINT` via `signalfd`, with no connection resets
- Path traversal blocked in-kernel by `openat2(RESOLVE_BENEATH)`, so there is no TOCTOU window
- Access log with client IP, request, status, bytes and handling time, buffered per worker and written by a separate thread so it never blocks an event loop
- An exception becomes a 500 for that one connection, it never takes down the worker
- Status codes: 200, 400, 403, 404, 405, 408, 413, 500, 501

## Architecture

The main thread only accepts. Each connection is handed to a worker and stays there,
so its buffers are touched by one thread and the request path needs no locking. The
access log is the only shared state.

```
clients --> accept loop       # main thread: accept() + signalfd
            |
            |-- worker 0      # epoll loop + thread, owns its connections
            |-- worker 1      # handed over round-robin via eventfd
            '-- worker N
```

Each worker's epoll set holds three kinds of fd, and `Worker::run()` dispatches on
whichever fired:

```
worker epoll set
  |-- eventfd     # new connections from the accept loop
  |-- timerfd     # 1s tick, sweeps connections idle past 10s
  '-- sockets     # one per live connection, EPOLLIN or EPOLLOUT
```

A connection is a two-state machine, driven entirely by readiness events. Nothing
blocks, and partial reads and writes resume on the next wakeup:

```
   +---------+   headers parsed,   +---------+
   | READING | ------------------> | WRITING |
   +---------+   response built    +---------+
        ^                               |
        '--------- keep-alive ----------'

   READING -> CLOSED   # peer closed, idle 10s (408), headers over 8KB (413)
   WRITING -> CLOSED   # response sent with Connection: close, or a write error
```

Where a request goes:

```
Worker::handle_readable                 # read bytes into the connection buffer
  '-- Worker::process_buffered          # find \r\n\r\n, enforce the 8KB cap
        '-- handle_request                                  # request_handler.cpp
              |-- parse_request_line, parse_headers         # http_parse.cpp
              |-- open_under_root, content_type_for         # static_file.cpp
              '-- build_response, build_chunked_headers     # http_response.cpp
```

Everything below `handle_request` takes bytes and returns bytes, with no sockets
involved. All I/O lives in `worker.cpp` and `main.cpp`.

## Requirements

- Linux 5.6+ (or WSL2), for `epoll`, `eventfd`, `timerfd`, `signalfd` and `openat2`
- C++20 compiler (developed against g++ 13), CMake 3.20+

## Building

```bash
cmake -S . -B build/debug   -DCMAKE_BUILD_TYPE=Debug    # ASan + UBSan
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release  # -O2, no sanitizers
cmake -S . -B build/tsan    -DCMAKE_BUILD_TYPE=Tsan     # TSan, can't link with ASan
cmake --build build/release
```

On WSL2, TSan's fixed shadow memory collides with a PIE binary's randomized load
address and aborts with `FATAL: ThreadSanitizer: unexpected memory mapping`. Run it
with ASLR off:

```bash
setarch "$(uname -m)" -R ./build/tsan/server
```

## Running

```bash
./build/release/server
# listening on http://localhost:8080 (doc root: "/srv/www", 12 event-loop workers, log: stdout)

curl http://localhost:8080/       # serves index.html from the document root
curl -v http://localhost:8080/    # with response headers
```

## Configuration

```
  -p, --port PORT        port to listen on (default 8080)
  -d, --docroot DIR      directory to serve files from (default www)
  -w, --workers N        event-loop threads (default: one per core)
  -l, --log PATH         access log file (default: stdout)
  -h, --help             show this message and exit
```

```bash
./build/release/server --port 8000 --docroot /srv/www --workers 4 --log /var/log/http.log
```

Bad values and a missing document root fail at startup instead of defaulting silently.
The banner and any errors go to stderr, which leaves stdout for the log.

### Access log

```
2026-08-17T15:41:01.449629Z 127.0.0.1 "GET /big.txt" 200 252137 3403us
timestamp (UTC, us)         client    request        status  bytes  handling time
```

Timestamps sort lexicographically. The duration covers a complete request being parsed
through to the last byte written, so it measures handling and not how long the client
took. A request too broken to parse logs `"- -"`.

A worker appends finished lines to a buffer it owns; a separate writer thread collects
those buffers every 50ms and writes them in one call. Nothing on the request path takes a
shared lock or makes a syscall to log, so a slow log destination can't stall an event
loop. A worker that gets more than 4MB ahead of the writer drops lines and says how many.

## Testing

```bash
./build/debug/server &
./build/debug/test_http_conformance                                # ~25s, two checks wait out the 10s timeout

./build/debug/test_graceful_shutdown ./build/debug/server SIGTERM  # starts and stops its own server
./build/debug/test_graceful_shutdown ./build/debug/server SIGINT

./build/debug/test_graceful_shutdown 'setarch $(uname -m) -R ./build/tsan/server' SIGTERM
```

Two black-box binaries, built by the same project but linking nothing from `src/`. They
only speak HTTP, so they test the server the way a client sees it. 28 conformance checks
and 6 shutdown checks pass on Debug, Release and Tsan. Between them they cover the things
`curl` cannot express: half-sent requests, two requests in a single `write()`, chunked
bodies decoded byte for byte, idle timeouts, and signalling the server while 40 concurrent
keep-alive clients are mid-flight.

## Benchmarks

1KB file over keep-alive connections, server pinned to 6 CPUs (3 physical cores plus SMT
siblings) on an i7-1355U, load generator pinned to the other 6. Median of 3 runs, 30s
measured after a 10s warmup.

| connections | 1 worker | 6 workers | p50 | p99 | p99.9 |
|---:|---:|---:|---:|---:|---:|
| 1 | 18,302 | 14,800 | 0.04 ms | 0.14 ms | 0.36 ms |
| 50 | 91,301 | 209,022 | 0.18 ms | 0.58 ms | 0.81 ms |
| 100 | 98,127 | 276,372 | 0.27 ms | 0.90 ms | 1.23 ms |
| 500 | 101,868 | **305,005** | 1.30 ms | 3.63 ms | 4.79 ms |
| 1000 | 82,367 | 256,887 | 3.22 ms | 6.77 ms | 8.15 ms |

Latencies are the 6-worker column. Across 72 runs and 255.7M requests nothing was refused,
reset or dropped. At peak the server holds 5.98 of its 6 pinned cores, so it is the limit
rather than the load generator. Memory is 4.2 MB idle and 14 MB at peak throughput, most of
the difference being access-log buffers.

The two columns use different load-generator thread counts, because one setting cannot keep
both configurations server-bound: 6 workers need 4 client threads to reach saturation, while
1 worker is measured most favourably with 2. Both are verified server-bound from the CPU
column. [benchmarks/results.md](benchmarks/results.md) has the argument and the raw runs.

Two rounds of profiling account for most of the throughput:

- **Path resolution.** 7 of 17 syscalls per request went to canonicalization, where
  `fs::weakly_canonical` issued five `readlink` calls that all failed. `openat2(RESOLVE_BENEATH)`
  cut that to 2 of 11.5, raised throughput 14-16% (42% single-worker), and moved containment
  into the kernel.
- **The access log.** It held a global mutex and made a blocking `write()` on the event loop,
  which capped the server at 5.2 of 6 cores however much load arrived. Per-worker buffers
  drained by a writer thread lifted 6-worker throughput **96-179%**.

```bash
./benchmarks/run.sh                                   # ~25 min
python3 benchmarks/summarize.py benchmarks/raw.csv 30
```

Full method and per-run data: [benchmarks/results.md](benchmarks/results.md).

## Layout

```
src/
  main.cpp                 # listener, signalfd, accept loop, worker fan-out
  config.{h,cpp}           # CLI parsing and --help
  logging.{h,cpp}          # access log and stderr diagnostics
  worker.{h,cpp}           # epoll loop and per-connection state machine
  connection_state.h       # per-connection buffers, stage, timestamps
  request_handler.{h,cpp}  # parsed request in, response out
  http_parse.{h,cpp}       # request-line and header parsing
  http_response.{h,cpp}    # response and chunk formatting
  static_file.{h,cpp}      # openat2 path resolution, MIME types
tests/                     # black-box HTTP tests
benchmarks/                # load generator, sweep driver, results
www/                       # default document root
```

`www/big.txt` and `www/bench.txt` are generated by CMake, not checked in.
