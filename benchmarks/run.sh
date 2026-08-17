#!/usr/bin/env bash
# sweeps concurrency against worker counts, server and client on separate cores.
#   benchmarks/run.sh [output.csv]   (repo root, Release build)
set -euo pipefail

SERVER=./build/release/server
LOADGEN=./build/release/loadgen
OUT=${1:-benchmarks/raw.csv}

SERVER_CORES=${SERVER_CORES:-0-5}
CLIENT_CORES=${CLIENT_CORES:-6-11}
WORKER_COUNTS=${WORKER_COUNTS:-"1 6"}
CONCURRENCY=${CONCURRENCY:-"1 10 50 100 500 1000"}
REPEATS=${REPEATS:-3}
DURATION=${DURATION:-30}
WARMUP=${WARMUP:-10}
CLIENT_THREADS=${CLIENT_THREADS:-2}

for binary in "$SERVER" "$LOADGEN"; do
  [[ -x $binary ]] || { echo "missing $binary - build the Release config first" >&2; exit 1; }
done

echo "workers,connections,repeat,rps,p50_us,p95_us,p99_us,p999_us,max_us,requests,errors,cpu_s,peak_rss_kb" > "$OUT"

total=$(( $(wc -w <<<"$WORKER_COUNTS") * $(wc -w <<<"$CONCURRENCY") * REPEATS ))
run=0
started=$(date +%s)

for workers in $WORKER_COUNTS; do
  for conns in $CONCURRENCY; do
    for repeat in $(seq 1 "$REPEATS"); do
      run=$((run + 1))

      taskset -c "$SERVER_CORES" "$SERVER" --workers "$workers" --log /dev/null 2>/dev/null &
      server_pid=$!
      sleep 1

      threads=$CLIENT_THREADS
      [[ $conns -lt $threads ]] && threads=$conns

      row=$(taskset -c "$CLIENT_CORES" "$LOADGEN" \
              --connections "$conns" --duration "$DURATION" --warmup "$WARMUP" \
              --threads "$threads" --pid "$server_pid")

      kill "$server_pid" 2>/dev/null || true
      wait "$server_pid" 2>/dev/null || true

      echo "$workers,$conns,$repeat,${row#*,}" >> "$OUT"  # loadgen repeats conns back
      printf '[%2d/%2d] workers=%-2s conns=%-4s repeat=%s  %s rps\n' \
             "$run" "$total" "$workers" "$conns" "$repeat" "$(cut -d, -f2 <<<"$row")"
      sleep 1  # let TIME_WAIT drain
    done
  done
done

elapsed=$(( $(date +%s) - started ))
echo "done in $((elapsed / 60))m$((elapsed % 60))s -> $OUT"
