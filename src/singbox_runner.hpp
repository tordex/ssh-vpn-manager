#pragma once

#include <string>
#include <vector>

#include <sys/types.h>

// Helpers for running the system `sing-box` binary. Unlike ssh_runner there is
// no password/askpass plumbing (sing-box needs no secrets); supervision reuses
// the generic ssh_runner::terminate / read_stderr helpers.
namespace singbox_runner
{

    // Spawns `sing-box` with the given argv (argv[0] is searched on PATH). stdin
    // and stdout are wired to /dev/null; stderr is captured to a pipe. The child
    // is placed in its own session/process group so it can be signalled as a unit.
    //
    // Returns the child pid on success and sets *stderr_fd to the read end of the
    // stderr pipe (owned by the caller). Returns -1 on failure (errno set).
    pid_t spawn(const std::vector<std::string>& args, int* stderr_fd);

} // namespace singbox_runner
