#include "transport/child_process.hpp"

#include <mx/errors.hpp>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace mx::detail {
namespace {

void closeIfOpen(int &fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

/// Turns a vector of strings into the NULL-terminated char* array exec wants.
/// The strings must outlive the returned pointers.
std::vector<char *> toArgArray(std::vector<std::string> &strings) {
    std::vector<char *> pointers;
    pointers.reserve(strings.size() + 1);
    for (std::string &s : strings) {
        pointers.push_back(s.data());
    }
    pointers.push_back(nullptr);
    return pointers;
}

} // namespace

struct ChildProcessTransport::Impl {
    pid_t pid = -1;
    int stdinWrite = -1;
    int stdoutRead = -1;
    bool closed = false;
    bool reaped = false;
};

ChildProcessTransport::ChildProcessTransport(const std::vector<std::string> &argv,
                                             const std::vector<EnvOverride> &env)
    : impl_(std::make_unique<Impl>()) {
    if (argv.empty()) {
        throw KernelError("ChildProcessTransport requires at least an executable");
    }

    int inPipe[2] = {-1, -1};
    int outPipe[2] = {-1, -1};
    int statusPipe[2] = {-1, -1};

    const auto closeAll = [&] {
        for (int *pipe : {inPipe, outPipe, statusPipe}) {
            closeIfOpen(pipe[0]);
            closeIfOpen(pipe[1]);
        }
    };

    if (::pipe(inPipe) != 0 || ::pipe(outPipe) != 0 || ::pipe(statusPipe) != 0) {
        closeAll();
        throw KernelError("Failed to create pipes for child process");
    }

    // Unlike CreateProcess, fork+exec cannot report a failed exec to the parent
    // directly. The classic remedy: a pipe marked close-on-exec. A successful
    // exec closes it and the parent's read returns 0; a failed one writes errno
    // through it first, so the failure is still synchronous.
    if (::fcntl(statusPipe[1], F_SETFD, FD_CLOEXEC) != 0) {
        closeAll();
        throw KernelError("Failed to configure the exec status pipe");
    }

    // argv and the environment are built here rather than between fork and
    // exec: allocation in the child is not async-signal-safe.
    std::vector<std::string> argvStorage = argv;
    std::vector<char *> argvPointers = toArgArray(argvStorage);
    std::vector<std::string> envStorage = mergeEnvironment(env);
    std::vector<char *> envPointers = toArgArray(envStorage);

    const pid_t pid = ::fork();
    if (pid < 0) {
        closeAll();
        throw KernelError("Failed to fork for child process");
    }

    if (pid == 0) {
        // Child. Only async-signal-safe calls from here to exec.
        ::dup2(inPipe[0], STDIN_FILENO);
        ::dup2(outPipe[1], STDOUT_FILENO);
        // stderr joins stdout, matching the Windows transport so that Maxima's
        // diagnostics arrive in the same stream as its results.
        ::dup2(outPipe[1], STDERR_FILENO);

        ::close(inPipe[0]);
        ::close(inPipe[1]);
        ::close(outPipe[0]);
        ::close(outPipe[1]);
        ::close(statusPipe[0]);

        ::execve(argvPointers[0], argvPointers.data(), envPointers.data());

        const int failure = errno;
        // Best effort: if this write fails the parent sees a 0-byte read and
        // the child's exit status still reports the problem.
        const ssize_t ignored
            = ::write(statusPipe[1], &failure, sizeof(failure));
        static_cast<void>(ignored);
        ::_exit(127);
    }

    // Parent.
    closeIfOpen(inPipe[0]);
    closeIfOpen(outPipe[1]);
    closeIfOpen(statusPipe[1]);

    int execErrno = 0;
    ssize_t received = 0;
    do {
        received = ::read(statusPipe[0], &execErrno, sizeof(execErrno));
    } while (received < 0 && errno == EINTR);
    closeIfOpen(statusPipe[0]);

    if (received > 0) {
        int discarded = 0;
        ::waitpid(pid, &discarded, 0);
        closeIfOpen(inPipe[1]);
        closeIfOpen(outPipe[0]);
        throw KernelError("Failed to start child process: " + argv.front() + ": "
                          + std::strerror(execErrno));
    }

    impl_->pid = pid;
    impl_->stdinWrite = inPipe[1];
    impl_->stdoutRead = outPipe[0];
}

ChildProcessTransport::~ChildProcessTransport() {
    kill();
}

void ChildProcessTransport::send(std::string_view bytes) {
    if (impl_->closed || impl_->stdinWrite < 0) {
        return;
    }
    size_t written = 0;
    while (written < bytes.size()) {
        const ssize_t n = ::write(impl_->stdinWrite, bytes.data() + written,
                                  bytes.size() - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            // A closed pipe means the child is gone; receive()/alive() report
            // it, so there is nothing useful to do here.
            return;
        }
        written += static_cast<size_t>(n);
    }
}

std::string ChildProcessTransport::receive(std::chrono::milliseconds timeout) {
    if (impl_->closed || impl_->stdoutRead < 0) {
        return {};
    }

    // poll() takes the deadline directly, so unlike the Windows transport there
    // is no polling loop and no sleep.
    pollfd descriptor{};
    descriptor.fd = impl_->stdoutRead;
    descriptor.events = POLLIN;

    const int ready = ::poll(&descriptor, 1, static_cast<int>(timeout.count()));
    if (ready < 0) {
        if (errno == EINTR) {
            return {}; // Interrupted; the caller's own deadline still applies.
        }
        impl_->closed = true;
        return {};
    }
    if (ready == 0) {
        return {}; // Timed out with nothing available; alive() stays true.
    }

    char chunk[4096];
    const ssize_t bytesRead = ::read(impl_->stdoutRead, chunk, sizeof(chunk));
    if (bytesRead < 0) {
        if (errno == EINTR) {
            return {};
        }
        impl_->closed = true;
        return {};
    }
    if (bytesRead == 0) {
        impl_->closed = true; // End of stream: the child has exited.
        return {};
    }
    return std::string(chunk, static_cast<size_t>(bytesRead));
}

bool ChildProcessTransport::alive() const {
    if (impl_->closed || impl_->pid <= 0 || impl_->reaped) {
        return false;
    }
    int status = 0;
    const pid_t result = ::waitpid(impl_->pid, &status, WNOHANG);
    if (result == 0) {
        return true;
    }
    // Either it exited (result == pid) or it cannot be waited for; either way
    // remember that, so the pid is never waited on twice.
    impl_->reaped = true;
    return false;
}

void ChildProcessTransport::kill() {
    // Closing stdin gives a child that was asked to quit a chance to see EOF
    // and leave on its own.
    closeIfOpen(impl_->stdinWrite);

    if (impl_->pid > 0 && !impl_->reaped) {
        const auto deadline
            = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        for (;;) {
            int status = 0;
            const pid_t result = ::waitpid(impl_->pid, &status, WNOHANG);
            if (result != 0) {
                break; // Exited, or cannot be waited for.
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                ::kill(impl_->pid, SIGKILL);
                int discarded = 0;
                ::waitpid(impl_->pid, &discarded, 0);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        impl_->reaped = true;
    }

    closeIfOpen(impl_->stdoutRead);
    impl_->pid = -1;
    impl_->closed = true;
}

} // namespace mx::detail
