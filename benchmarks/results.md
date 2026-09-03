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
| Client threads | 4 for the 6-worker sweep, 2 for the 1-worker sweep - see below |

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release && cmake --build build/release
CLIENT_THREADS=4 ./benchmarks/run.sh benchmarks/raw.csv
python3 benchmarks/summarize.py benchmarks/raw.csv 30
```

Raw runs:

| file | build | client threads |
|---|---|---:|
| `raw.csv` | current | 4 |
| `raw-client2.csv` | current | 2 |
| `raw-before-logging.csv` | before the access-log change | 4 |
| `raw-before-logging-client2.csv` | before the access-log change | 2 |
| `raw-before-openat2.csv`, `raw-openat2.csv` | the earlier `openat2` comparison | 2 |

The `openat2` numbers were measured months earlier on a quieter machine and are not
directly comparable with the rest; they are kept because the before/after within that
pair is.

Each run starts a fresh server, drives it with `benchmarks/loadgen.cpp`, and
records the raw row in `raw.csv`. The load generator is closed-loop: every
connection keeps exactly one request in flight and sends the next only after the
response is complete, so reported latency is a full request/response round trip
and throughput is a consequence of it rather than an independently set rate.

Latency is bucketed at 1µs resolution. CPU comes from the server's
`/proc/<pid>/stat` sampled across the measured window only, and peak RSS from
`VmHWM`, so both describe the server process rather than the machine.

### Is the load generator the bottleneck?

This check has to be redone whenever the server gets materially faster, and that
is exactly what went wrong here.

The original check held the client fixed at 2 threads and scaled the server:

| workers | req/s | server CPU (cores) |
|---:|---:|---:|
| 1 | 49,069 | 1.37 |
| 2 | 98,496 | 2.71 |
| 4 | 113,004 | 5.06 |
| 6 | 123,940 | 5.82 |

Throughput tracked worker count until the server's pinned CPUs saturated, which is
the signature of the server being the limit, and adding client threads at fixed
server settings *lowered* throughput. So 2 client threads was right for a server
that topped out near 124k req/s.

After the access-log change the server was roughly twice as fast, and that
conclusion silently expired. Re-running the check at 500 connections, 6 workers:

| client threads | req/s | server CPU (cores) |
|---:|---:|---:|
| 2 | 268,032 | 5.32 |
| 4 | **321,655** | **5.96** |
| 6 | 291,424 | 6.00 |

At 2 threads the server sits at 5.3 of its 6 pinned cores - it has capacity it is
not being given, so the number measures the client. At 4 the server is pinned at
5.96/6.00 and the measurement is of the server. At 6 the client's own overhead
starts costing more than it adds.

**There is no single setting that keeps both configurations server-bound.** With 1
worker, 4 client threads makes things *worse* (64k vs 98k at 100 connections):
the server thread is saturated at 1.00 core either way, but spreading the same
connections over more client loops scatters arrivals, so each `epoll_wait`
returns fewer events and per-request overhead rises.

So each configuration is swept at the client setting that leaves the server the
bottleneck, and the CPU column in the results below is the evidence:

| configuration | client threads | server CPU at peak | bound by |
|---|---:|---:|---|
| 1 worker | 2 | 1.00 / 1.00 | server |
| 6 workers | 4 | 5.98 / 6.00 | server |
| 6 workers | 2 | 5.32 / 6.00 | *client* - not used |

## Results

72 runs of the current build, 255,716,302 requests, **zero errors** - nothing
refused, reset or dropped at any concurrency, including 1000 connections. The
"errors" counter is a transport counter: failed connects, `EPOLLHUP`/`EPOLLERR`,
failed `send`/`recv`. The generator does not inspect status codes, so this is a
claim about connections, not about response codes.

### 1 worker (2 client threads)

| connections | req/s | p50 | p95 | p99 | p99.9 | max | CPU (cores) | peak RSS |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 18,302 | 0.03 ms | 0.07 ms | 0.15 ms | 0.43 ms | 9.97 ms | 0.52 | 4.3 MB |
| 10 | 67,915 | 0.13 ms | 0.21 ms | 0.26 ms | 0.52 ms | 4.37 ms | 0.98 | 4.7 MB |
| 50 | 91,301 | 0.53 ms | 0.72 ms | 0.90 ms | 1.39 ms | 5.24 ms | 1.00 | 5.2 MB |
| 100 | 98,127 | 0.99 ms | 1.28 ms | 1.57 ms | 2.58 ms | 9.64 ms | 1.00 | 5.3 MB |
| 500 | 101,868 | 4.40 ms | 7.76 ms | 12.21 ms | 16.43 ms | 28.05 ms | 1.00 | 6.3 MB |
| 1000 | 82,367 | 11.01 ms | 18.21 ms | 21.81 ms | 26.29 ms | 35.03 ms | 1.00 | 7.6 MB |

### 6 workers (4 client threads)

| connections | req/s | p50 | p95 | p99 | p99.9 | max | CPU (cores) | peak RSS |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 14,800 | 0.04 ms | 0.08 ms | 0.14 ms | 0.36 ms | 5.18 ms | 0.54 | 4.3 MB |
| 10 | 65,918 | 0.10 ms | 0.22 ms | 0.28 ms | 0.40 ms | 4.33 ms | 3.52 | 5.8 MB |
| 50 | 209,022 | 0.18 ms | 0.44 ms | 0.58 ms | 0.81 ms | 6.40 ms | 5.65 | 9.6 MB |
| 100 | 276,372 | 0.27 ms | 0.69 ms | 0.90 ms | 1.23 ms | 8.65 ms | 5.89 | 10.9 MB |
| 500 | 305,005 | 1.30 ms | 2.95 ms | 3.63 ms | 4.79 ms | 28.34 ms | 5.98 | 14.1 MB |
| 1000 | 256,887 | 3.22 ms | 6.06 ms | 6.77 ms | 8.15 ms | 13.55 ms | 5.99 | 12.6 MB |

### Scaling

Each column at the client setting that leaves the server the bottleneck:

| connections | 1 worker | 6 workers | speedup |
|---:|---:|---:|---:|
| 1 | 18,302 | 14,800 | 0.81x |
| 10 | 67,915 | 65,918 | 0.97x |
| 50 | 91,301 | 209,022 | 2.29x |
| 100 | 98,127 | 276,372 | 2.82x |
| 500 | 101,868 | 305,005 | 2.99x |
| 1000 | 82,367 | 256,887 | 3.12x |

Reading these together:

- **A single worker saturates around 100k req/s**, its CPU pegged at 1.00 core
  from 10 connections upward. Throughput plateaus while latency grows with
  concurrency: a queue forming in front of a busy server.
- **Six workers reach ~305k req/s** at 500 connections and hold ~257k at 1000.
- **Speedup is ~3x, not 6x.** The server's 6 pinned CPUs are 3 physical cores plus
  SMT siblings, so ~3x is close to what the hardware can give - and it is reached
  only once the access log stopped serialising the workers.
- **At 1 connection more workers do not help**, and can hurt: one request is in
  flight at a time, so there is nothing to parallelise and the extra threads only
  add scheduling noise.
- **Memory grows with throughput, not just connections**: 4.2 MB idle, 14.1 MB at
  305k req/s. The difference is access-log buffers, which are bounded at 4 MB per
  worker before lines start being dropped.

### Run-to-run spread

Median 5.7%, and very unevenly distributed. The 6-worker points that carry the
headline are tight - 1.4% at 100 connections, 1.2% at 500, 2.0% at 1000. The wide
points are all at low concurrency (11.7% at 1 connection) or on the 1-worker
column at high concurrency (36-58%).

Both are inherent to the configuration rather than noise more runs would remove.
At 1 connection the measurement is latency-bound: one request in flight, so a
single scheduling delay lands directly on throughput. With one worker at high
concurrency, one stalled thread holds up every connection behind it. The 6-worker
column, which is the headline, is stable.

## The access log was the bottleneck

Every worker wrote its access line inside a global mutex, with a blocking
`write()`, on the event loop thread. Measured against the same sweep on the same
machine, 6 workers and 4 client threads throughout:

| connections | before | after | gain | before CPU | after CPU |
|---:|---:|---:|---:|---:|---:|
| 1 | 14,271 | 14,800 | **+4%** | 0.57 | 0.54 |
| 10 | 60,354 | 65,918 | **+9%** | 3.92 | 3.52 |
| 50 | 106,421 | 209,022 | **+96%** | 5.26 | 5.65 |
| 100 | 109,271 | 276,372 | **+153%** | 5.27 | 5.89 |
| 500 | 109,183 | 305,005 | **+179%** | 5.24 | 5.98 |
| 1000 | 112,125 | 256,887 | **+129%** | 5.24 | 5.99 |

The CPU column is the diagnosis. Before the change the server sits at **5.24-5.27
cores no matter how much load arrives** - 50, 100, 500 and 1000 connections all
produce the same ~110k req/s at the same CPU. It is not short of work; it cannot
use the cores it has, because 12 threads are serialising through one mutex. After,
it reaches 5.98/6.00 and throughput tracks concurrency again.

The same change at 1 worker (2 client threads, where there is no contention to
remove) is worth far less:

| connections | before | after | gain |
|---:|---:|---:|---:|
| 1 | 12,965 | 18,302 | +41% |
| 10 | 56,602 | 67,915 | +20% |
| 50 | 73,830 | 91,301 | +24% |
| 100 | 89,529 | 98,127 | +10% |
| 500 | 93,109 | 101,868 | +9% |
| 1000 | 88,343 | 82,367 | -7% |

**+9-41% single-threaded against +96-179% at six workers.** A gain that grows with
thread count is contention, not syscall cost; if the `write()` itself had been the
expense, both would have improved by a similar fraction. The single point that got
worse - 1 worker at 1000 connections, -7% - has 13% run-to-run spread, so it is at
the noise floor rather than a real regression.

Logging now appends to a buffer each worker owns; one writer thread collects every
sink every 50ms and does the I/O. The request path takes an uncontended per-thread
mutex and makes no syscall. Verified complete rather than merely fast: 1,970,733
log lines written for 1,970,659 timed requests at 197k req/s, zero dropped.

## Syscall profile

`perf` is unavailable on this kernel (no `linux-tools` build for WSL2, and
`ptrace_scope=1` blocks attaching after the fact), so this is `strace -f -c` with
the server launched as a child, at 6 workers and 100 connections.

Tracing slows the server by ~66x, so the *timings* in `strace` output are
meaningless here. The **call counts** are the result:

| syscall | original | after `openat2` | today | what it is |
|---|---:|---:|---:|---|
| `readlink` | 5.0 | **0** | 0 | path canonicalization; every call failed with EINVAL |
| `newfstatat` | 2.0 | 0 | 0 | canonicalization + the `is_regular_file` check |
| `fstat` | 0 | 1.0 | 1.0 | ruling out directories, and now sizing the read |
| `openat` / `openat2` | 1.0 | 1.0 | 1.0 | opening the file |
| `read` | 3.0 | 3.0 | **2.0** | 1 socket read + 1 file read; `fstat` removed the EOF probe |
| `write` | 2.0 | 2.0 | **1.0** | 1 socket write; the log write is now batched away |
| `epoll_ctl` | 2.0 | 2.0 | 2.0 | switching interest to write, then back to read |
| `close` | 1.0 | 1.0 | 1.0 | closing the file |
| **total** | **17.0** | **11.5** | **~10** | |

The "today" column is measured at 1 connection, where `epoll_wait` runs 2.0 per
request rather than amortising across a batch; the comparable figures for the
other two columns are 18.0 and 12.0.

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
kept in `benchmarks/raw-before-openat2.csv`.

**These are the numbers as they stood in that session**, before the access-log
change and on a quieter machine - the 149,581 below was the headline at the time
and is not comparable with the 305,005 in Results above:

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

Both of the costs flagged here at the time have since been removed: the access log
took a `write` even to `/dev/null`, and the file took 2 `read` calls because the
code read a block and then read again to discover EOF. Buffering the log and
sizing the read from `fstat` account for the drop from 11.5 syscalls to ~10, and
for most of the throughput above.

What is left: `sendfile()` would remove the remaining file `read` and the
userspace copy, and edge-triggered `epoll` would remove both `epoll_ctl` calls.

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
- **The client setting is part of the measurement.** Two of the sweeps here were
  invalidated by it: 2 client threads was correct for the pre-`openat2` server and
  wrong for the current one. Any future change that moves throughput materially
  invalidates the check in "Is the load generator the bottleneck?" and it has to
  be redone.
- **`openat2` numbers are from an earlier session.** They were taken on a quieter
  machine; the unmodified build scores ~110k at 6 workers/100 connections today
  against 129k then. Compare within a pair, never across.
- **Latency is measured closed-loop**, one request in flight per connection, so
  the percentiles are subject to coordinated omission: when the server stalls the
  generator stops sending, and the requests that would have queued are never
  issued or measured. The tail here is optimistic. An open-loop generator with
  rate-scheduled send times would be needed to quote p99 without that caveat.
