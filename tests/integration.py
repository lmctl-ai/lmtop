#!/usr/bin/env python3
"""lmtop integration test: drive a real server over stdio NDJSON.

Usage: python3 tests/integration.py [build-dir-or-binary]

Sequence: launch the server, generate CPU and disk load, monitor for 10
seconds, fetch metrics, verify the JSON data is *plausible* (not just
well-formed), then shut the server down and require a clean exit.

On Linux every check runs against live samples. On Windows/macOS the same
checks run against the platform backends (PDH / mach+IOKit); PSI gauges are
-1 by design outside Linux and are skipped by the range checks.
"""
import glob
import json
import math
import os
import subprocess
import sys
import tempfile
import threading
import time

INTERVAL_MS = 200
MONITOR_SECONDS = 10
PCT_METRICS = (
    "cpu.usage_pct", "cpu.user_pct", "cpu.system_pct", "cpu.iowait_pct",
    "cpu.max_core_busy_pct", "cpu.psi_some_avg10",
    "mem.used_pct", "mem.swap_used_pct", "mem.psi_some_avg10",
    "disk.max_util_pct", "disk.psi_some_avg10",
)
RATE_METRICS = (
    "disk.read_bytes_rate", "disk.write_bytes_rate",
    "disk.read_ops_rate", "disk.write_ops_rate",
    "net.rx_bytes_rate", "net.tx_bytes_rate",
    "net.rx_packets_rate", "net.tx_packets_rate",
    "net.rx_errors_rate", "net.tx_errors_rate",
    "net.rx_dropped_rate", "net.tx_dropped_rate",
)


def find_binary(root: str) -> str:
    if os.path.isfile(root):
        return root
    candidates = []
    for pattern in ("lmtop", "lmtop.exe"):
        candidates += glob.glob(os.path.join(root, "**", pattern), recursive=True)
    candidates = [c for c in candidates if os.path.isfile(c) and "CMakeFiles" not in c]
    if not candidates:
        sys.exit(f"error: no lmtop binary under {root}")
    return sorted(candidates, key=len)[0]


def send(proc, request_id, method, params=None):
    req = {"id": request_id, "method": method}
    if params is not None:
        req["params"] = params
    proc.stdin.write(json.dumps(req) + "\n")
    proc.stdin.flush()
    while True:
        line = proc.stdout.readline()
        if not line:
            raise RuntimeError("server closed stdout mid-conversation")
        response = json.loads(line)
        if response.get("id") == request_id:
            return response


def expect(cond, message):
    if not cond:
        raise AssertionError(message)


def busy_cpu(seconds):
    """One hot thread: gives cpu.user_pct something real to measure."""
    end = time.monotonic() + seconds
    acc = 0
    while time.monotonic() < end:
        acc += 1
    return acc


def disk_write(path, mib=32):
    """A real fsync'd file write so disk rates are nonzero somewhere."""
    block = os.urandom(4 * 1024 * 1024)
    with open(path, "wb") as handle:
        for _ in range(mib // 4):
            handle.write(block)
        handle.flush()
        os.fsync(handle.fileno())
    os.remove(path)


def check_now(now):
    values = now["values"]
    expect(len(values) == 24, "now returns 24 metric values")
    expect(all(isinstance(v, (int, float)) for v in values.values()),
           "all values are numbers (-1 marks missing)")

    for name in PCT_METRICS:
        v = values[name]
        expect(v == -1 or 0.0 <= v <= 100.0, f"{name}={v} outside [0,100]")
    for name in RATE_METRICS:
        v = values[name]
        expect(v == -1 or v >= 0.0, f"{name}={v} negative rate")
    expect(values["mem.available_bytes"] > 0, "memory available is positive")

    # Identities that must hold on every OS: the exposed slices reassemble
    # to the busy share — user + system(=kernel incl. irq/steal) + iowait
    # equals usage (100 - idle). On Windows iowait is 0 (no equivalent);
    # system there already includes interrupt+dpc.
    usage = values["cpu.usage_pct"]
    user, system, iowait = (values["cpu.user_pct"], values["cpu.system_pct"],
                            values["cpu.iowait_pct"])
    expect(usage != -1 and user != -1, "cpu aggregates present")
    expect(abs(user + system + iowait - usage) <= 1.0,
           f"user({user})+system({system})+iowait({iowait}) != usage({usage})")

    now_ms = now["unix_ms"]
    expect(abs(now_ms - int(time.time() * 1000)) < 24 * 3600 * 1000,
           "unix_ms is a real wall-clock millisecond timestamp")
    # Note: no per-sample "max core >= usage" identity here. The aggregate
    # "cpu" line and the per-core lines are accounted independently by the
    # kernel, so on near-idle machines a one-tick discrepancy is normal.
    # The load-bearing check lives in check_summary, on the loaded window.


def check_summary(summary):
    expect(summary["samples"] >= MONITOR_SECONDS * 1000 // INTERVAL_MS - 10,
           f"summary saw only {summary['samples']} samples in "
           f"{MONITOR_SECONDS}s at {INTERVAL_MS}ms")
    for m in summary["metrics"]:
        if m["n"] > 0:
            expect(m["min"] <= m["mean"] <= m["max"], f"{m['name']} min<=mean<=max")
            expect(m["min"] <= m["median"] <= m["max"], f"{m['name']} min<=median<=max")
            expect(m["min"] <= m["p95"] <= m["max"], f"{m['name']} min<=p95<=max")
            expect(m["stddev"] >= 0, f"{m['name']} stddev >= 0")
    # The busy thread ran inside this window: cpu usage must have moved.
    cpu = next(m for m in summary["metrics"] if m["name"] == "cpu.usage_pct")
    expect(cpu["max"] > 5.0, f"cpu.usage_pct max {cpu['max']} — load not detected")
    mem = next(m for m in summary["metrics"] if m["name"] == "mem.used_pct")
    expect(0.0 <= mem["mean"] <= 100.0, "mem.used_pct mean in [0,100]")

    # Per-core identity, on the loaded window where it is noise-immune:
    # one busy thread pegs a core near 100%, and the hottest core's peak
    # must dominate the aggregate peak. This is what actually validates the
    # per-core parsing on every OS.
    max_core = next(m for m in summary["metrics"]
                    if m["name"] == "cpu.max_core_busy_pct")
    if max_core["n"] > 0:
        expect(max_core["max"] > 50.0,
               f"max_core peak {max_core['max']} — busy thread not visible per-core")
        expect(max_core["max"] + 2.0 >= cpu["max"],
               f"max core peak {max_core['max']} < aggregate peak {cpu['max']}")


def check_history(history, interval_ms):
    expect(history["total_rows"] >= MONITOR_SECONDS * 1000 // interval_ms - 10,
           f"history kept only {history['total_rows']} rows in "
           f"{MONITOR_SECONDS}s")
    columns = history["columns"]
    expect(len(columns) == 26, "columns = unix_ms + dt_s + 24 metrics")
    expect(columns[0] == "unix_ms" and columns[1] == "dt_s", "column order")
    i_t, i_dt = columns.index("unix_ms"), columns.index("dt_s")
    i_cpu = columns.index("cpu.usage_pct")

    rows = history["rows"]
    expect(len(rows) >= 2, "at least two rows to compare")
    lo, hi = interval_ms / 1000.0 * 0.5, interval_ms / 1000.0 * 2.0
    for k, row in enumerate(rows):
        expect(lo <= row[i_dt] <= hi, f"row {k} dt_s {row[i_dt]} outside "
                                      f"[{lo},{hi}]")
        expect(0.0 <= row[i_cpu] <= 100.0, f"row {k} cpu.usage_pct out of range")
        if k > 0:
            expect(row[i_t] > rows[k - 1][i_t], "timestamps strictly increase")
    return rows[-1][i_t]  # newest sample's timestamp


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "build"
    binary = find_binary(root)
    proc = subprocess.Popen(
        [binary, "server", "--interval", str(INTERVAL_MS)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    request_id = 0

    def call(method, params=None):
        nonlocal request_id
        request_id += 1
        return send(proc, request_id, method, params)

    try:
        # --- launch + status ------------------------------------------------
        res = call("status")["result"]
        expect(res["protocol"] == "lmtop/1", "protocol tag")
        expect(len(res["metrics"]) == 24, "24 metric catalog rows")
        supported = res.get("supported", True)
        platform = res.get("platform", "linux")

        if supported:
            # --- monitor 10 s with real load on cpu and disk -----------------
            started = time.monotonic()
            worker = threading.Thread(target=busy_cpu, args=(3.0,), daemon=True)
            worker.start()
            with tempfile.NamedTemporaryFile(delete=False, suffix=".load") as tmp:
                tmp_path = tmp.name
            try:
                disk_write(tmp_path)
            finally:
                if os.path.exists(tmp_path):
                    os.remove(tmp_path)
            while time.monotonic() - started < MONITOR_SECONDS:
                time.sleep(0.1)

            # --- get metrics -------------------------------------------------
            now = call("now")["result"]
            check_now(now)
            summary = call("summary", {"seconds": MONITOR_SECONDS})["result"]
            check_summary(summary)
            history = call("history",
                           {"seconds": MONITOR_SECONDS,
                            "max_points": 250})["result"]
            newest = check_history(history, INTERVAL_MS)

            # Downsampling: stride ceil(total/max_points), last row kept, and
            # the kept last row is the same sample as the full history's end.
            small = call("history",
                         {"seconds": MONITOR_SECONDS, "max_points": 8})["result"]
            expect(small["total_rows"] == history["total_rows"],
                   "same window, same total_rows")
            expect(small["stride"] >= max(1, -(-small["total_rows"] // 8)),
                   "stride covers total/max_points")
            expect(len(small["rows"]) <= 9, "rows bounded by max_points + 1")
            expect(small["rows"][-1][0] == newest, "downsampled last row is the "
                                                   "newest sample")
        else:
            for method, params in (("now", None), ("summary", {"seconds": 1}),
                                   ("history", {"seconds": 1})):
                res = call(method, params)["result"]
                expect("error" in res, f"{method} reports unsupported on {platform}")

        # --- malformed request ------------------------------------------------
        proc.stdin.write("this is not json\n")
        proc.stdin.flush()
        line = proc.stdout.readline()
        expect(line, "server answered the malformed line")
        expect("error" in json.loads(line), "malformed line -> error envelope")

        # --- unknown method ---------------------------------------------------
        response = call("bogus")
        expect("error" in response, "unknown method -> error envelope")

        # --- shutdown ----------------------------------------------------------
        res = call("stop")["result"]
        expect(res.get("stopping") is True, "stop acknowledged")
        proc.stdin.close()
        rc = proc.wait(timeout=15)
        expect(rc == 0, f"clean exit, got {rc}")

        print(f"OK  platform={platform}  supported={supported}  "
              f"monitored={MONITOR_SECONDS}s  binary={binary}")
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()


if __name__ == "__main__":
    main()
