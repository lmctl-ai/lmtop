#pragma once
// Newline-delimited JSON protocol over stdio: one request object per line on
// stdin, one response object per line on stdout. Diagnostics go to stderr.
// The server runs until stdin reaches EOF or a "stop" request succeeds.
//
// Request:  {"id": <int>, "method": "<name>", "params": {...}}
// Response: {"id": <int>, "result": {...}}  or  {"id": <int>, "error": "..."}

#include <cstddef>
#include <cstdint>
#include <string>

namespace lmtop {

// interval_ms: sampling period (clamped to [100, 60000]).
// cap_mb: history byte budget in MiB (converted inside).
int run_server(int64_t interval_ms, size_t cap_mb);

} // namespace lmtop
