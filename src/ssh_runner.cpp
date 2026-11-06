#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "ssh_runner.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace ssh_runner {

namespace {

// Owns the askpass helper file for the lifetime of the process and removes it
// on shutdown. The file itself contains no secret (it echoes an environment
// variable), but cleaning it up is still tidier.
struct AskpassFile {
  std::string path;
  ~AskpassFile() {
    if (!path.empty()) {
      unlink(path.c_str());
    }
  }
};

AskpassFile make_askpass() {
  AskpassFile result;

  char tmpl[] = "/tmp/ssh-vpn-manager-askpass-XXXXXX";
  const int fd = mkstemp(tmpl);
  if (fd < 0) {
    return result;
  }

  static const char kScript[] =
      "#!/bin/sh\n"
      "printf '%s' \"$SSH_VPN_MANAGER_PASSWORD\"\n";
  const ssize_t written = write(fd, kScript, sizeof(kScript) - 1);
  if (fchmod(fd, 0700) != 0 || written != static_cast<ssize_t>(sizeof(kScript) - 1)) {
    close(fd);
    unlink(tmpl);
    return result;
  }
  close(fd);
  result.path = tmpl;
  return result;
}

std::vector<std::string> build_env(const std::string& password) {
  std::vector<std::string> env;
  bool have_askpass = false;
  bool have_require = false;
  bool have_pw = false;

  const std::string& script = askpass_path();
  for (char** e = environ; e && *e; ++e) {
    std::string line = *e;
    if (line.rfind("SSH_ASKPASS=", 0) == 0) {
      line = "SSH_ASKPASS=" + script;
      have_askpass = true;
    } else if (line.rfind("SSH_ASKPASS_REQUIRE=", 0) == 0) {
      line = "SSH_ASKPASS_REQUIRE=force";
      have_require = true;
    } else if (line.rfind("SSH_VPN_MANAGER_PASSWORD=", 0) == 0) {
      line = "SSH_VPN_MANAGER_PASSWORD=" + password;
      have_pw = true;
    }
    env.push_back(std::move(line));
  }
  if (!have_askpass) env.push_back("SSH_ASKPASS=" + script);
  if (!have_require) env.push_back("SSH_ASKPASS_REQUIRE=force");
  if (!have_pw) env.push_back("SSH_VPN_MANAGER_PASSWORD=" + password);
  return env;
}

}  // namespace

const std::string& askpass_path() {
  static const AskpassFile file = make_askpass();
  return file.path;
}

pid_t spawn(const std::vector<std::string>& args, const std::string& password,
            int* stderr_fd) {
  if (stderr_fd) *stderr_fd = -1;
  if (args.empty() || askpass_path().empty()) {
    errno = EINVAL;
    return -1;
  }

  int err_pipe[2];
  if (pipe2(err_pipe, O_CLOEXEC) != 0) {
    return -1;
  }

  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_adddup2(&fa, err_pipe[1], 2);
  posix_spawn_file_actions_addclose(&fa, err_pipe[0]);
  posix_spawn_file_actions_addclose(&fa, err_pipe[1]);

  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);

  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (const auto& a : args) {
    argv.push_back(const_cast<char*>(a.c_str()));
  }
  argv.push_back(nullptr);

  std::vector<std::string> env = build_env(password);
  std::vector<char*> envp;
  envp.reserve(env.size() + 1);
  for (auto& e : env) {
    envp.push_back(const_cast<char*>(e.c_str()));
  }
  envp.push_back(nullptr);

  pid_t pid = -1;
  const int rc =
      posix_spawnp(&pid, argv[0], &fa, &attr, argv.data(), envp.data());

  posix_spawn_file_actions_destroy(&fa);
  posix_spawnattr_destroy(&attr);
  close(err_pipe[1]);

  if (rc != 0) {
    close(err_pipe[0]);
    errno = rc;
    return -1;
  }

  // Make the read end nonblocking so read_stderr() never stalls the caller.
  const int fl = fcntl(err_pipe[0], F_GETFL, 0);
  if (fl >= 0) {
    fcntl(err_pipe[0], F_SETFL, fl | O_NONBLOCK);
  }

  if (stderr_fd) {
    *stderr_fd = err_pipe[0];
  } else {
    close(err_pipe[0]);
  }
  return pid;
}

ssize_t read_stderr(int fd, std::string& out) {
  if (fd < 0) {
    return 0;
  }
  char buf[4096];
  ssize_t total = 0;
  for (;;) {
    const ssize_t n = read(fd, buf, sizeof buf);
    if (n > 0) {
      out.append(buf, static_cast<std::size_t>(n));
      total += n;
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    break;  // EAGAIN (no more buffered), EOF, or error
  }
  return total;
}

void terminate(pid_t pid, int grace_ms) {
  if (pid <= 0) {
    return;
  }
  kill(-pid, SIGTERM);

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(grace_ms);
  int status = 0;
  for (;;) {
    const pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) {
      return;
    }
    if (r < 0) {
      if (errno == EINTR) continue;
      return;  // already reaped elsewhere
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      break;
    }
    usleep(50 * 1000);
  }

  kill(-pid, SIGKILL);
  for (;;) {
    const pid_t r = waitpid(pid, &status, 0);
    if (r == pid) break;
    if (r < 0 && errno != EINTR) break;
  }
}

std::string error_from_stderr(const std::string& stderr_text) {
  // Prefer a line matching one of these specific patterns (ordered by how
  // actionable they are); otherwise fall back to the last non-empty line.
  static const char* const kPatterns[] = {
      "Address already in use",
      "Permission denied",
      "Host key verification failed",
      "Could not resolve hostname",
      "Connection refused",
      "Connection timed out",
      "No route to host",
  };

  std::string last;  // last non-empty line (fallback)
  std::string best;
  int best_prio = -1;

  std::size_t start = 0;
  while (start <= stderr_text.size()) {
    const std::size_t nl = stderr_text.find('\n', start);
    const std::size_t end = (nl == std::string::npos) ? stderr_text.size() : nl;
    std::string line = stderr_text.substr(start, end - start);

    // Trim trailing whitespace.
    while (!line.empty() &&
           (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
      line.pop_back();
    }

    if (!line.empty()) {
      last = line;
      const int n = static_cast<int>(sizeof(kPatterns) / sizeof(kPatterns[0]));
      for (int i = 0; i < n; ++i) {
        if (line.find(kPatterns[i]) != std::string::npos) {
          if (best_prio < 0 || i <= best_prio) {
            best = line;
            best_prio = i;
          }
          break;
        }
      }
    }
    if (nl == std::string::npos) break;
    start = nl + 1;
  }

  std::string result = best_prio >= 0 ? best : last;

  // Strip a leading "ssh: " for a friendlier message.
  if (result.rfind("ssh: ", 0) == 0) {
    result.erase(0, 5);
  }
  return result;
}

}  // namespace ssh_runner
