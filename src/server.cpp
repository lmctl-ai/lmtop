#include "server.hpp"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <poll.h>
#include <string>
#include "json.hpp"
#include "monitor.hpp"

namespace lmtop {
namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) {
  g_stop = true;
}

// Extracts an optional positive-int param; returns false when present but
// invalid, true when absent or valid (value 0 = "not given" for optional
// params).
bool param_int(const Json *params, const char *name, int64_t &value,
               std::string &error) {
  value = 0;
  if (!params) return true;
  const Json *p = params->find(name);
  if (!p || p->is_null()) return true;
  if (!p->is_number() || p->as_int(-1) < 0) {
    error = std::string("invalid params.") + name + ": expected non-negative int";
    return false;
  }
  value = p->as_int();
  return true;
}

class Server {
public:
  Server(int64_t interval_ms, size_t cap_mb) : interval_ms_(interval_ms), cap_mb_(cap_mb) {}

  int run() {
    std::string error;
    if (!monitor_.start(interval_ms_, cap_mb_ * 1024 * 1024, error)) {
      std::fprintf(stderr, "lmtop: cannot start monitor: %s\n", error.c_str());
      return 1;
    }
    std::fprintf(stderr,
                 "lmtop: sampling every %lld ms, history cap %zu MiB; "
                 "waiting for requests on stdin\n",
                 static_cast<long long>(interval_ms_), cap_mb_);

    // POSIX: poll() with a timeout instead of a blocking getline — signal
    // handlers only set a flag (std::cin retries through EINTR internally),
    // so the timeout is what guarantees shutdown is observed promptly.
    // Windows: no signals to speak of; the blocking getline plus `stop` and
    // EOF is sufficient.
    auto serve_line = [this](std::string &line) -> int {
      if (line.empty()) return 0;
      const std::string response = handle(line);
      std::cout << response << '\n';
      std::cout.flush();
      if (std::cout.bad()) return 4; // stdout write failure
      return 0;
    };
#if defined(_WIN32)
    std::string line;
    while (!g_stop) {
      if (!std::getline(std::cin, line)) break; // EOF or error
      if (const int rc = serve_line(line)) {
        monitor_.stop();
        return rc;
      }
    }
#else
    std::string line;
    while (!g_stop) {
      pollfd fds{STDIN_FILENO, POLLIN, 0};
      const int ready = poll(&fds, 1, 200);
      if (ready < 0) {
        if (errno == EINTR) continue;
        break;
      }
      if (ready == 0) continue;
      if (!std::getline(std::cin, line)) break; // EOF or error
      if (const int rc = serve_line(line)) {
        monitor_.stop();
        return rc;
      }
    }
#endif
    monitor_.stop();
    return 0;
  }

private:
  std::string handle(const std::string &line) {
    Json request;
    std::string error;
    if (!parse(line, request, error))
      return response(Json(), Json(error));
    if (!request.is_object())
      return response(Json(), Json("request must be a JSON object"));
    const Json *id = request.find("id");
    const Json *method = request.find("method");
    if (!method || !method->is_string())
      return response(id ? *id : Json(), Json("missing method"));
    const Json *params = request.find("params");
    if (params && !params->is_object())
      return response(id ? *id : Json(), Json("params must be an object"));

    const std::string &name = method->as_string();
    Json result;
    if (name == "status") {
      result = monitor_.status_json();
    } else if (name == "now") {
      result = monitor_.now_json();
    } else if (name == "history") {
      int64_t seconds = 0, max_points = 0;
      if (!param_int(params, "seconds", seconds, error) ||
          !param_int(params, "max_points", max_points, error))
        return response(id ? *id : Json(), Json(error));
      result = monitor_.history_json(seconds, max_points);
    } else if (name == "summary") {
      int64_t seconds = 0;
      if (!param_int(params, "seconds", seconds, error))
        return response(id ? *id : Json(), Json(error));
      result = monitor_.summary_json(seconds);
    } else if (name == "stop") {
      result = Json::obj({{"stopping", true}});
      const std::string out = response(id ? *id : Json(), result);
      g_stop = true; // respond first, then exit the loop
      return out;
    } else {
      return response(id ? *id : Json(), Json("unknown method: " + name));
    }
    return response(id ? *id : Json(), result);
  }

  static std::string response(const Json &id, const Json &result_or_error) {
    Json out;
    if (result_or_error.is_string()) // convention: string payload = error
      out = Json::obj({{"id", id}, {"error", result_or_error}});
    else
      out = Json::obj({{"id", id}, {"result", result_or_error}});
    return dump(out);
  }

  Monitor monitor_;
  int64_t interval_ms_;
  size_t cap_mb_;
};

} // namespace

int run_server(int64_t interval_ms, size_t cap_mb) {
#if !defined(_WIN32)
  // No SA_RESTART: a signal must interrupt the blocked stdin read so the
  // loop can observe g_stop and shut down cleanly.
  struct sigaction action {};
  action.sa_handler = on_signal;
  sigemptyset(&action.sa_mask);
  sigaction(SIGINT, &action, nullptr);
  sigaction(SIGTERM, &action, nullptr);
#endif
  return Server(interval_ms, cap_mb).run();
}

} // namespace lmtop
