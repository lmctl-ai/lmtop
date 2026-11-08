#include "client.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "json.hpp"

namespace lmtop {
namespace {

struct Options {
  int64_t id = 1;
  int64_t interval_ms = 1000;
  std::string in_path, out_path; // both set = FIFO mode
  std::string method, params;
};

int usage(FILE *stream) {
  std::fprintf(stream,
               "usage: lmtop client [--id N] [--interval MS] <method> [params-json]\n"
               "       lmtop client [--id N] --in PATH --out PATH <method> [params-json]\n");
  return stream == stdout ? 0 : 2;
}

bool write_all(int fd, const std::string &data) {
  size_t off = 0;
  while (off < data.size()) {
    const ssize_t n = write(fd, data.data() + off, data.size() - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    off += static_cast<size_t>(n);
  }
  return true;
}

// Extracts the next complete line from `pending`, if any.
bool next_line(std::string &pending, std::string &line) {
  const auto nl = pending.find('\n');
  if (nl == std::string::npos) return false;
  line = pending.substr(0, nl);
  pending.erase(0, nl + 1);
  return true;
}

bool is_matching_response(const std::string &line, int64_t id) {
  Json response;
  std::string error;
  if (!parse(line, response, error) || !response.is_object()) return false;
  const Json *rid = response.find("id");
  return rid && rid->is_number() && rid->as_int() == id;
}

// Reads until EOF (spawn mode: the child stays alive, so this is bounded by
// the matching response arriving; the caller stops the server afterwards).
// Keeps the LAST line whose id matches, in case stale duplicates exist.
bool read_until_match(int fd, int64_t id, std::string &reply) {
  std::string pending, line;
  char buf[8192];
  bool matched = false;
  while (true) {
    const ssize_t n = read(fd, buf, sizeof buf);
    if (n < 0) {
      if (errno == EINTR) continue;
      return matched;
    }
    if (n == 0) return matched;
    pending.append(buf, static_cast<size_t>(n));
    while (next_line(pending, line)) {
      if (is_matching_response(line, id)) {
        reply = line;
        matched = true;
      }
    }
    if (matched) return true;
  }
}

// --- Spawn mode -------------------------------------------------------------

int spawn_mode(const Options &opt, const std::string &request) {
  int to_child[2], from_child[2];
  if (pipe(to_child) != 0 || pipe(from_child) != 0) {
    std::fprintf(stderr, "lmtop client: pipe: %s\n", std::strerror(errno));
    return 1;
  }
  const pid_t pid = fork();
  if (pid < 0) {
    std::fprintf(stderr, "lmtop client: fork: %s\n", std::strerror(errno));
    return 1;
  }
  if (pid == 0) {
    dup2(to_child[0], STDIN_FILENO);
    dup2(from_child[1], STDOUT_FILENO);
    close(to_child[0]);
    close(to_child[1]);
    close(from_child[0]);
    close(from_child[1]);
    const std::string interval = std::to_string(opt.interval_ms);
    execl("/proc/self/exe", "lmtop", "server", "--interval", interval.c_str(),
          static_cast<char *>(nullptr));
    _exit(127);
  }
  close(to_child[0]);
  close(from_child[1]);

  int rc = 1;
  std::string reply;
  if (!write_all(to_child[1], request + "\n")) {
    std::fprintf(stderr, "lmtop client: cannot write request: %s\n",
                 std::strerror(errno));
  } else if (read_until_match(from_child[0], opt.id, reply)) {
    const std::string stop =
        dump(Json::obj({{"id", opt.id + 1}, {"method", "stop"}}));
    write_all(to_child[1], stop + "\n");
    std::printf("%s\n", reply.c_str());
    rc = 0;
  } else {
    std::fprintf(stderr, "lmtop client: no response from server\n");
  }
  close(to_child[1]);
  close(from_child[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  return rc;
}

// --- FIFO mode --------------------------------------------------------------

int fifo_mode(const Options &opt, const std::string &request) {
  const int in = open(opt.in_path.c_str(), O_WRONLY);
  if (in < 0) {
    std::fprintf(stderr, "lmtop client: cannot open %s: %s\n",
                 opt.in_path.c_str(), std::strerror(errno));
    return 1;
  }
  if (!write_all(in, request + "\n")) {
    std::fprintf(stderr, "lmtop client: cannot write request: %s\n",
                 std::strerror(errno));
    close(in);
    return 1;
  }
  close(in); // one request per open keeps the line framing clean on FIFOs

  const int out = open(opt.out_path.c_str(), O_RDONLY | O_NONBLOCK);
  if (out < 0) {
    std::fprintf(stderr, "lmtop client: cannot open %s: %s\n",
                 opt.out_path.c_str(), std::strerror(errno));
    return 1;
  }
  std::string pending, line, reply;
  bool matched = false;
  while (true) {
    // After a match, a short idle timeout means the tail is fully drained.
    const int timeout = matched ? 200 : 3000;
    pollfd pfd{out, POLLIN, 0};
    const int ready = poll(&pfd, 1, timeout);
    if (ready < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (ready == 0) break;
    char buf[8192];
    const ssize_t n = read(out, buf, sizeof buf);
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN) continue;
      break;
    }
    if (n == 0) break;
    pending.append(buf, static_cast<size_t>(n));
    while (next_line(pending, line)) {
      if (is_matching_response(line, opt.id)) {
        reply = line; // keep the newest match
        matched = true;
      }
    }
  }
  close(out);

  if (reply.empty()) {
    std::fprintf(stderr,
                 "lmtop client: no response with id %lld in %s (is the server "
                 "running and the request id fresh?)\n",
                 static_cast<long long>(opt.id), opt.out_path.c_str());
    return 1;
  }
  std::printf("%s\n", reply.c_str());
  return 0;
}

} // namespace

int run_client(const std::vector<std::string> &args) {
  Options opt;
  std::vector<std::string> positional;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string &a = args[i];
    auto next = [&](const char *flag) -> const std::string * {
      if (i + 1 >= args.size()) {
        std::fprintf(stderr, "lmtop client: missing value for %s\n", flag);
        return nullptr;
      }
      return &args[++i];
    };
    if (a == "--id" || a == "--interval" || a == "--in" || a == "--out") {
      const std::string *v = next(a.c_str());
      if (!v) return usage(stderr);
      if (a == "--in") {
        opt.in_path = *v;
      } else if (a == "--out") {
        opt.out_path = *v;
      } else {
        char *end = nullptr;
        const long long value = std::strtoll(v->c_str(), &end, 10);
        if (!end || *end != '\0') return usage(stderr);
        if (a == "--id") {
          if (value < 0) return usage(stderr);
          opt.id = value;
        } else {
          if (value <= 0) return usage(stderr);
          opt.interval_ms = value;
        }
      }
    } else if (a == "--help") {
      return usage(stdout);
    } else if (!a.empty() && a[0] == '-') {
      std::fprintf(stderr, "lmtop client: unknown flag: %s\n", a.c_str());
      return usage(stderr);
    } else {
      positional.push_back(a);
    }
  }
  if (positional.empty() || positional.size() > 2) return usage(stderr);
  if (opt.in_path.empty() != opt.out_path.empty()) {
    std::fprintf(stderr, "lmtop client: --in and --out must be given together\n");
    return usage(stderr);
  }
  opt.method = positional[0];
  if (positional.size() == 2) opt.params = positional[1];

  Json params;
  if (!opt.params.empty()) {
    std::string error;
    if (!parse(opt.params, params, error)) {
      std::fprintf(stderr, "lmtop client: invalid params json: %s\n",
                   error.c_str());
      return 2;
    }
    if (!params.is_object()) {
      std::fprintf(stderr, "lmtop client: params must be a JSON object\n");
      return 2;
    }
  }
  Json request = Json::obj({{"id", opt.id}, {"method", opt.method}});
  if (!opt.params.empty()) request.set("params", params);

  if (!opt.in_path.empty()) return fifo_mode(opt, dump(request));
  return spawn_mode(opt, dump(request));
}

} // namespace lmtop
