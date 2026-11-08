#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "client.hpp"
#include "server.hpp"

namespace {

int usage(FILE *stream) {
  std::fprintf(stream,
               "lmtop - top-like resource monitor as a stdio JSON server\n"
               "\n"
               "usage:\n"
               "  lmtop server [--interval MS] [--cap-mb N]   run the monitor server\n"
               "                                                (default command)\n"
               "  lmtop client [options] <method> [params]    one-shot CLI query\n"
               "\n"
               "server protocol: one JSON request per line on stdin, one JSON response\n"
               "per line on stdout. methods: status, now, history, summary, stop.\n");
  return stream == stdout ? 0 : 2;
}

int server_main(const std::vector<std::string> &args) {
  long long interval_ms = 1000;
  long long cap_mb = 100;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string &a = args[i];
    auto next = [&](const char *flag) -> const char * {
      if (i + 1 >= args.size()) {
        std::fprintf(stderr, "lmtop server: missing value for %s\n", flag);
        return nullptr;
      }
      return args[++i].c_str();
    };
    const char *v = nullptr;
    if (a == "--interval") {
      if (!(v = next("--interval"))) return usage(stderr);
      interval_ms = std::atoll(v);
    } else if (a == "--cap-mb") {
      if (!(v = next("--cap-mb"))) return usage(stderr);
      cap_mb = std::atoll(v);
    } else if (a == "--help") {
      return usage(stdout);
    } else {
      std::fprintf(stderr, "lmtop server: unknown argument: %s\n", a.c_str());
      return usage(stderr);
    }
  }
  if (interval_ms < 100 || interval_ms > 60000) {
    std::fprintf(stderr, "lmtop server: --interval must be 100..60000 ms\n");
    return 2;
  }
  if (cap_mb < 1) {
    std::fprintf(stderr, "lmtop server: --cap-mb must be >= 1\n");
    return 2;
  }
  return lmtop::run_server(interval_ms, static_cast<size_t>(cap_mb));
}

} // namespace

int main(int argc, char **argv) {
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);

  if (!args.empty() && args[0] == "--help") return usage(stdout);
  if (!args.empty() && args[0] == "client")
    return lmtop::run_client({args.begin() + 1, args.end()});
  if (!args.empty() && args[0] == "server")
    return server_main({args.begin() + 1, args.end()});
  if (!args.empty() && args[0].rfind("-", 0) == 0) {
    std::fprintf(stderr, "lmtop: unknown argument: %s\n", args[0].c_str());
    return usage(stderr);
  }
  // No command: run the server (optionally with flags, e.g. `lmtop --interval 500`).
  return server_main(args);
}
