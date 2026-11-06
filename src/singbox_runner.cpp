#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "singbox_runner.hpp"

#include <cerrno>

#include <fcntl.h>
#include <spawn.h>
#include <unistd.h>

extern char** environ;

namespace singbox_runner
{

    pid_t spawn(const std::vector<std::string>& args, int* stderr_fd)
    {
        if(stderr_fd)
        {
            *stderr_fd = -1;
        }
        if(args.empty())
        {
            errno = EINVAL;
            return -1;
        }

        int err_pipe[2];
        if(pipe2(err_pipe, O_CLOEXEC) != 0)
        {
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
        for(const auto& a : args)
        {
            argv.push_back(const_cast<char*>(a.c_str()));
        }
        argv.push_back(nullptr);

        pid_t     pid = -1;
        const int rc  = posix_spawnp(&pid, argv[0], &fa, &attr, argv.data(), environ);

        posix_spawn_file_actions_destroy(&fa);
        posix_spawnattr_destroy(&attr);
        close(err_pipe[1]);

        if(rc != 0)
        {
            close(err_pipe[0]);
            errno = rc;
            return -1;
        }

        // Make the read end nonblocking so read_stderr() never stalls the caller.
        const int fl = fcntl(err_pipe[0], F_GETFL, 0);
        if(fl >= 0)
        {
            fcntl(err_pipe[0], F_SETFL, fl | O_NONBLOCK);
        }

        if(stderr_fd)
        {
            *stderr_fd = err_pipe[0];
        } else
        {
            close(err_pipe[0]);
        }
        return pid;
    }

} // namespace singbox_runner
