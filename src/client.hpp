#pragma once
// CLI access to an lmtop server. Two modes:
//
// Spawn mode (default): starts a private `lmtop server` child, sends one
// request, prints the response, and stops the server. State lives only for
// the call, so `now`/`status` are the useful methods here.
//
// FIFO mode (--in PATH --out PATH): talks to a long-lived supervised server
// whose stdio is wired to a FIFO pair, e.g.
//   mkfifo /tmp/lmtop.in /tmp/lmtop.out
//   lmtop server < /tmp/lmtop.in > /tmp/lmtop.out 2> lmtop.log &
//   lmtop client --in /tmp/lmtop.in --out /tmp/lmtop.out summary '{"seconds":300}'
// History persists in the server process for as long as the supervisor
// keeps it alive.

#include <string>
#include <vector>

namespace lmtop {

int run_client(const std::vector<std::string> &args);

} // namespace lmtop
