#!/usr/bin/env python3
"""lmtop integration test: drive a real server over stdio NDJSON.

Usage: python3 tests/integration.py [build-dir-or-binary]

On Linux every check runs against live samples. On Windows/macOS the server
builds and answers the protocol with supported=false (lmdebug convention);
there the checks assert the graceful unsupported envelope instead.
"""
import glob
import json
import os
import subprocess
import sys
import time


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


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "build"
    binary = find_binary(root)
    proc = subprocess.Popen(
        [binary, "server", "--interval", "200"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    try:
        # --- status -------------------------------------------------------------
        res = send(proc, 1, "status")["result"]
        expect(res["protocol"] == "lmtop/1", "protocol tag")
        expect(len(res["metrics"]) == 24, "24 metric catalog rows")
        expect("memory_bytes_cap" in res, "cap reported")
        supported = res.get("supported", True)
        platform = res.get("platform", "linux")

        if supported:
            # --- now ------------------------------------------------------------
            res = send(proc, 2, "now")["result"]
            expect("values" in res, "now returns values")
            expect(len(res["values"]) == 24, "24 live values")
            expect(all(isinstance(v, (int, float)) for v in res["values"].values()),
                   "values are numbers (-1 marks missing)")

            time.sleep(0.4)

            # --- summary ----------------------------------------------------------
            res = send(proc, 3, "summary", {"seconds": 1})["result"]
            expect(res["samples"] >= 1, "summary window non-empty")
            for m in res["metrics"]:
                if m["n"] > 0:
                    expect(m["min"] <= m["mean"] <= m["max"], f"{m['name']} min<=mean<=max")
                    expect(m["min"] <= m["median"] <= m["max"], f"{m['name']} min<=median<=max")
                    expect(m["min"] <= m["p95"] <= m["max"], f"{m['name']} min<=p95<=max")
                    expect(m["stddev"] >= 0, f"{m['name']} stddev >= 0")

            # --- history ----------------------------------------------------------
            res = send(proc, 4, "history", {"seconds": 1, "max_points": 3})["result"]
            expect(res["total_rows"] >= 1, "history window non-empty")
            expect(len(res["rows"]) >= 1, "history returns rows")
            expect(len(res["columns"]) == 26, "columns = unix_ms + dt_s + 24")
            expect(res["columns"][0] == "unix_ms", "first column is unix_ms")
        else:
            # Unsupported platform: sampling methods answer with the envelope.
            for request_id, method, params in (
                (2, "now", None),
                (3, "summary", {"seconds": 1}),
                (4, "history", {"seconds": 1}),
            ):
                res = send(proc, request_id, method, params)["result"]
                expect("error" in res, f"{method} reports unsupported on {platform}")

        # --- malformed request ----------------------------------------------------
        proc.stdin.write("this is not json\n")
        proc.stdin.flush()
        line = proc.stdout.readline()
        expect(line, "server answered the malformed line")
        expect("error" in json.loads(line), "malformed line -> error envelope")

        # --- unknown method -------------------------------------------------------
        response = send(proc, 90, "bogus")
        expect("error" in response, "unknown method -> error envelope")

        # --- stop -------------------------------------------------------------------
        res = send(proc, 99, "stop")["result"]
        expect(res.get("stopping") is True, "stop acknowledged")
        proc.stdin.close()
        rc = proc.wait(timeout=15)
        expect(rc == 0, f"clean exit, got {rc}")

        print(f"OK  platform={platform}  supported={supported}  binary={binary}")
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()


if __name__ == "__main__":
    main()
