# Benchmarks

Throughput and latency serving a 1KB static file over keep-alive connections,
sweeping concurrency against worker count.

## Method

| | |
|---|---|
| CPU | 13th Gen Intel Core i7-1355U |
| Topology seen by the guest | 12 logical CPUs over 6 physical cores (SMT pairs 0/1, 2/3, …) |
| Memory | 7.6 GiB |
| Kernel | 6.18.33.2-microsoft-standard-WSL2 |
| Compiler | g++ 13.3.0, `-O2`, sanitizers off |
| Server cores | 0–5 (`taskset`) |
| Load generator cores | 6–11 (`taskset`) |
| Payload | `www/bench.txt`, 1024 bytes |
| Per run | 10s warmup discarded, then 30s measured |
| Repeats | 3 per point, median reported |

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release && cmake --build build/release
./benchmarks/run.sh benchmarks/raw.csv
python3 benchmarks/summarize.py benchmarks/raw.csv 30
```

Each run starts a fresh server, drives it with `benchmarks/loadgen.cpp`, and
records the raw row in `raw.csv`. The load generator is closed-loop: every
connection keeps exactly one request in flight and sends the next only after the
response is complete, so reported latency is a full request/response round trip
and throughput is a consequence of it rather than an independently set rate.

Latency is bucketed at 1µs resolution. CPU comes from the server's
`/proc/<pid>/stat` sampled across the measured window only, and peak RSS from
`VmHWM`, so both describe the server process rather than the machine.

### Is the load generator the bottleneck?

Checked before running the sweep, by holding the client fixed and scaling the
server:

| workers | req/s | server CPU (cores) |
|---:|---:|---:|
| 1 | 49,069 | 1.37 |
| 2 | 98,496 | 2.71 |
| 4 | 113,004 | 5.06 |
| 6 | 123,940 | 5.82 |

Throughput tracks worker count until the server's pinned CPUs are saturated,
which is the signature of the server being the limit. Adding *client* threads at
fixed server settings lowered throughput rather than raising it, so the client
side has headroom it isn't using.

## Results

38 runs, 91,911,755 requests, **zero errors** — nothing refused, reset or
dropped at any concurrency, including 1000 connections.

### 1 worker

| connections | req/s | p50 | p95 | p99 | p99.9 | max | CPU (cores) | peak RSS |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 13,449 | 0.03 ms | 0.15 ms | 0.28 ms | 0.55 ms | 11.36 ms | 0.63 | 4.1 MB |
| 10 | 59,029 | 0.12 ms | 0.35 ms | 0.76 ms | 1.17 ms | 17.04 ms | 1.08 | 4.2 MB |
| 50 | 65,189 | 0.57 ms | 1.65 ms | 3.45 ms | 12.33 ms | 52.00 ms | 1.11 | 4.2 MB |
| 100 | 76,313 | 1.05 ms | 2.44 ms | 4.60 ms | 9.45 ms | 42.08 ms | 1.11 | 4.2 MB |
| 500 | 76,817 | 5.88 ms | 10.26 ms | 13.96 ms | 25.38 ms | 69.08 ms | 1.11 | 4.8 MB |
| 1000 | 69,120 | 13.18 ms | 21.45 ms | 32.26 ms | 50.23 ms | 68.84 ms | 1.11 | 5.5 MB |

### 6 workers

| connections | req/s | p50 | p95 | p99 | p99.9 | max | CPU (cores) | peak RSS |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 14,816 | 0.03 ms | 0.13 ms | 0.22 ms | 0.49 ms | 4.94 ms | 0.62 | 4.2 MB |
| 10 | 60,160 | 0.11 ms | 0.24 ms | 0.33 ms | 0.49 ms | 23.91 ms | 3.37 | 4.9 MB |
| 50 | 141,108 | 0.30 ms | 0.64 ms | 0.82 ms | 1.15 ms | 19.13 ms | 5.26 | 4.9 MB |
| 100 | 149,581 | 0.62 ms | 1.17 ms | 1.48 ms | 2.15 ms | 26.73 ms | 5.43 | 4.8 MB |
| 500 | 144,880 | 3.36 ms | 4.87 ms | 5.96 ms | 8.71 ms | 49.20 ms | 5.50 | 5.2 MB |
| 1000 | 145,068 | 6.80 ms | 8.77 ms | 9.86 ms | 17.37 ms | 50.75 ms | 5.53 | 6.0 MB |

### Scaling

| connections | 1 worker | 6 workers | speedup |
|---:|---:|---:|---:|
| 1 | 13,449 | 14,816 | 1.10x |
| 10 | 59,029 | 60,160 | 1.02x |
| 50 | 65,189 | 141,108 | 2.16x |
| 100 | 76,313 | 149,581 | 1.96x |
| 500 | 76,817 | 144,880 | 1.89x |
| 1000 | 69,120 | 145,068 | 2.10x |

Reading these together:

- **A single worker saturates around 77k req/s**, its CPU pegged at 1.1 cores
  from 10 connections upward — one worker thread plus a little accept thread.
  Throughput plateaus while latency grows with concurrency: a queue forming in
  front of a busy server.
- **Six workers reach ~150k req/s** and hold ~145k out to 1000 connections
  rather than collapsing.
- **Speedup is ~2x, not 6x.** The server's 6 pinned CPUs are 3 physical cores
  plus SMT siblings, so ~2x is close to what the hardware can give.
- **Below 50 connections extra workers barely help** (1.02x at 10 connections)
  while costing 3x the CPU. Too few connections to spread means the work doesn't
  parallelize but the overhead still applies.
- **Latency is where worker count really shows.** At 1000 connections p99 drops
  from 32ms to 10ms.
- **Memory is flat and small**: 4.2 MB idle, 6.0 MB with 1000 live connections —
  roughly 2 KB of state per connection.

### Run-to-run spread

Median 7.2%, but very unevenly distributed. The 6-worker points are tight
(0.6–1.8% at 50 connections and above); the 1-worker points are not, reaching
38% at a single connection.

At 1 connection the measurement is latency-bound — one request in flight, so a
single scheduling delay lands directly on throughput. With one worker at high
concurrency, one stalled thread holds up every connection behind it. Both are
inherent to the configuration rather than noise that more runs would remove; the
1-connection point was sampled 5 times instead of 3 for that reason. The
6-worker column, which is the headline, is stable.

## Syscall profile

`perf` is unavailable on this kernel (no `linux-tools` build for WSL2, and
`ptrace_scope=1` blocks attaching after the fact), so this is `strace -f -c` with
the server launched as a child, at 6 workers and 100 connections.

Tracing slows the server by ~66x, so the *timings* in `strace` output are
meaningless here. The **call counts** are the result:

| syscall | per request | after `openat2` | what it is |
|---|---:|---:|---|
| `readlink` | 5.0 | **0** | path canonicalization; every call failed with EINVAL |
| `newfstatat` | 2.0 | 0 | canonicalization + the `is_regular_file` check |
| `fstat` | 0 | 1.0 | ruling out directories |
| `openat` / `openat2` | 1.0 | 1.0 | opening the file |
| `read` | 3.0 | 3.0 | 1 socket read + 2 to read the file and hit EOF |
| `write` | 2.0 | 2.0 | 1 socket write + 1 access log line |
| `epoll_ctl` | 2.0 | 2.0 | switching interest to write, then back to read |
| `close` | 1.0 | 1.0 | closing the file |
| **total** | **17.0** | **11.5** | |

The original profile showed **seven of seventeen syscalls per request went to
path resolution**, and the five `readlink` calls all failed — that was
`fs::weakly_canonical` walking each component looking for symlinks and finding
none, on every request. It cost more syscalls than reading the file did.

### What replaced it

`openat2(RESOLVE_BENEATH)` asks the kernel to open a path while refusing to
resolve outside a directory fd. Path resolution went from 7 syscalls to 2, and
the userspace canonicalize-then-check-then-open sequence collapsed into a single
kernel operation.

The security property improved at the same time. The old code resolved a path,
checked it was under the document root, and then opened it — three steps with a
window in between where a symlink could be swapped. `RESOLVE_BENEATH` is enforced
*during* resolution, so the check and the open are the same operation and there
is no window. `RESOLVE_NO_MAGICLINKS` also blocks `/proc/self/fd` style
indirection, which the old prefix check never considered.

Two behaviour changes, both improvements:

- A path longer than `PATH_MAX` now returns **400**. It used to throw out of
  `weakly_canonical` and surface as a 500.
- Traversal through a *non-existent* directory (`/a/../../etc/passwd`) returns
  404 rather than 403, because resolution stops at the missing component before
  reaching the `..`. Still refused.

### Throughput effect

Full sweep re-run after the change, same harness and method. Pre-change data is
kept in `benchmarks/raw-before-openat2.csv`:

| connections | 1 worker before | after | 6 workers before | after |
|---:|---:|---:|---:|---:|
| 1 | 11,217 | 13,449 (+20%) | 12,549 | 14,816 (+18%) |
| 10 | 45,222 | 59,029 (+31%) | 50,548 | 60,160 (+19%) |
| 50 | 51,694 | 65,189 (+26%) | 123,630 | 141,108 (+14%) |
| 100 | 55,822 | 76,313 (+37%) | 129,060 | 149,581 (+16%) |
| 500 | 53,990 | 76,817 (+42%) | 126,310 | 144,880 (+15%) |
| 1000 | 52,880 | 69,120 (+31%) | 124,766 | 145,068 (+16%) |

The single-worker gains are larger because that configuration is purely
CPU-bound on one thread, so removing a third of its syscalls converts almost
directly into throughput. At 6 workers the machine is closer to its memory and
scheduling limits, so the same saving yields less.

Remaining per-request costs worth noting: the access log takes a `write` even to
`/dev/null` (worth buffering), and reading the file takes 2 `read` calls because
the code reads a block and then reads again to discover EOF — `sendfile()` would
remove both those reads and the userspace copy.

## Caveats

- **Loopback only.** No network stack between client and server, so absolute
  throughput is higher and latency lower than any real deployment. The
  comparisons between configurations are the meaningful part, not the absolute
  numbers.
- **SMT, not 6 independent cores.** Pinning the server to CPUs 0–5 gives it 3
  physical cores plus their SMT siblings, so 6 workers oversubscribe the
  physical cores 2:1. This is why scaling flattens well before 6x.
- **WSL2.** The guest sees a flat, homogeneous topology; the host's actual
  P-core/E-core layout isn't exposed, and the hypervisor sits under every
  syscall. Numbers from bare metal would differ.
- **Client and server share a machine.** Pinning keeps them off each other's
  CPUs, but they still contend for memory bandwidth and last-level cache.
