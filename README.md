# lmtop

`top` as a stdio JSON server. lmtop is **not interactive**: it launches,
starts monitoring host resources (CPU, memory, disk, network), keeps the
history in memory (capped at ~100 MB by default), and answers requests
arriving as newline-delimited JSON on stdin. Every response is JSON.

The resource readers and the metric set are aligned with
[lmdebug](../lmdebug)'s `sys sample` / `sys counters` design
(`docs/design/sys-sampling.md`): same `/proc` and `/sys` sources, same disk
eligibility rules, same host-normalized percentages, same missing-value
convention (`-1`).

## Build

```sh
cmake -B build && cmake --build build -j
```

Requires a C++17 compiler and pthreads. Linux only (the readers are procfs).

## Running

### Server

```sh
lmtop server [--interval MS] [--cap-mb N]
```

- `--interval` sampling period, 100–60000 ms (default 1000). The schedule is
  absolute (`sleep_until`); missed ticks are skipped, never burst-caught-up,
  and counted in `status.missed_ticks`.
- `--cap-mb` byte budget for the in-memory history (default 100). Samples are
  fixed-size records; the oldest are evicted when the budget is full.

The server speaks one JSON request per line on stdin and writes one JSON
response per line on stdout. Diagnostics go to stderr. It exits on stdin EOF
or a `stop` request; SIGINT/SIGTERM shut it down cleanly.

### Client

One-shot query against a private server (starts one, asks, stops it):

```sh
lmtop client status
lmtop client now
lmtop client summary
```

Against a long-lived supervised server, so history accumulates across calls:

```sh
mkfifo /tmp/lmtop.in /tmp/lmtop.out
lmtop server --interval 1000 < /tmp/lmtop.in > /tmp/lmtop.out 2>/tmp/lmtop.log &
sleep 5
lmtop client --in /tmp/lmtop.in --out /tmp/lmtop.out summary '{"seconds": 300}'
lmtop client --in /tmp/lmtop.in --out /tmp/lmtop.out history '{"seconds": 60, "max_points": 60}'
```

`--id N` sets the request id (default 1; use a fresh id per call in FIFO mode).

## Protocol

Request: `{"id": <int>, "method": "<name>", "params": {...}}`
Response: `{"id": <int>, "result": {...}}` or `{"id": <int>, "error": "<msg>"}`

| method   | params                          | result |
|---|---|---|
| `status` | —                               | uptime, pid, hostname, interval, samples kept, missed ticks, history bytes used/cap, metric catalog |
| `now`    | —                               | latest sample: all metric values, per-group status, `cpu_count` |
| `history`| `seconds` (0 = all), `max_points` (optional downsampling cap; last sample always kept) | `columns` + `rows` time series |
| `summary`| `seconds` (0 = all)             | per-metric `n/min/mean/median/p95/p99/max/stddev/first/last` over the window |
| `stop`   | —                               | `{"stopping": true}`, then the server exits |

## Metrics

24 metrics in four groups, named with lmdebug counter paths. `rate` values
are per-second deltas between ticks over the measured interval; `gauge`
values are read at the tick. Missing values (reader failure, counter reset,
device appeared/disappeared, PSI not exposed) are emitted as `-1`.

| group | metrics |
|---|---|
| cpu | `cpu.usage_pct`, `cpu.user_pct`, `cpu.system_pct`, `cpu.iowait_pct`, `cpu.max_core_busy_pct`, `cpu.psi_some_avg10` |
| mem | `mem.used_pct`, `mem.available_bytes`, `mem.swap_used_pct`, `mem.psi_some_avg10` |
| disk | `disk.read_bytes_rate`, `disk.write_bytes_rate`, `disk.read_ops_rate`, `disk.write_ops_rate`, `disk.max_util_pct`, `disk.psi_some_avg10` |
| net | `net.rx_bytes_rate`, `net.tx_bytes_rate`, `net.rx_packets_rate`, `net.tx_packets_rate`, `net.rx_errors_rate`, `net.tx_errors_rate`, `net.rx_dropped_rate`, `net.tx_dropped_rate` |

Notes carried over from lmdebug:

- CPU percentages are host-normalized (0–100 across all cores);
  `cpu.max_core_busy_pct` exposes a single saturated core.
- Disk totals sum eligible devices only: loop/ram/zram rows, partitions, and
  stacked devices (a partition backing a mapper, or its aggregate parent) are
  excluded, so I/O is never counted twice.
- Network totals sum non-loopback links from `/proc/net/dev`.
- PSI values are kernel 10-second averages, not tick measurements.
- Statistics follow the lmdebug conventions: sample stddev (Welford),
  type-7 percentiles, invalid intervals excluded (never zero-filled). p95
  needs n ≥ 30 and p99 needs n ≥ 100 to mean much.

## Layout

- `src/json.*` — minimal JSON value/parser/serializer (one-line output)
- `src/metrics.hpp` — metric catalog and the fixed-size `Sample` record
- `src/procfs.*` — `/proc`, `/sys` readers (cpu, mem, disk, net, PSI)
- `src/stats.*` — window statistics
- `src/monitor.*` — sampling thread, history ring (~100 MB cap), response builders
- `src/server.*` — stdio line protocol
- `src/client.*` — CLI (spawn and FIFO modes)
