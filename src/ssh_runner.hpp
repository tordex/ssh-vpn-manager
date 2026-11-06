#pragma once

#include <string>
#include <vector>

#include <sys/types.h>

// Helpers for running the system `ssh` binary (OpenSSH) with a password
// supplied through an SSH_ASKPASS helper, so the password never appears in
// argv (visible via `ps`). Shared by the tunnel worker (long-running
// `ssh -N -D ...`) and the server checker (one-shot `ssh ... true`).
namespace ssh_runner {

// Lazily writes the askpass helper script (mode 0700) and returns its path.
// The script prints $SSH_VPN_MANAGER_PASSWORD on stdout; it contains no secret.
// Returns an empty string on failure (spawn() will then fail cleanly).
const std::string& askpass_path();

// Spawns `ssh` with the given argv (argv[0] is searched on PATH). Supplies
// `password` via the askpass helper; stdin/stdout are wired to /dev/null and
// stderr is captured to a pipe so callers can surface ssh's error messages.
//
// On success returns the child pid (placed in its own session/process group so
// it can be signalled as a unit) and sets *stderr_fd to the read end of the
// stderr pipe (owned by the caller; close when done). On failure returns -1 and
// leaves errno set to the spawn error.
pid_t spawn(const std::vector<std::string>& args, const std::string& password,
            int* stderr_fd);

// Nonblocking drain of the stderr pipe into `out` (appends). Returns the number
// of bytes appended (0 when there is nothing buffered / EOF / pipe closed).
ssize_t read_stderr(int fd, std::string& out);

// SIGTERMs the child's process group, waits up to `grace_ms`, then SIGKILLs and
// reaps it. Safe to call only while the child is still running.
void terminate(pid_t pid, int grace_ms = 2000);

// Extracts a user-facing message from accumulated ssh stderr output (last
// non-empty line, with a leading "ssh: " stripped).
std::string error_from_stderr(const std::string& stderr_text);

}  // namespace ssh_runner
