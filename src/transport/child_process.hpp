#pragma once

#include "transport/itransport.hpp"
#include "transport/process_env.hpp"

#include <memory>
#include <string>
#include <vector>

namespace mx::detail {

/// Runs a child process with its stdin and stdout (and stderr) bound to pipes.
///
/// Generic: it is handed an argv and moves bytes. Nothing here knows what
/// Maxima or SBCL are, which is what keeps the Maxima-specific launch details
/// in MaximaSession where they can be tested against FakeTransport.
///
/// Arguments are supplied unquoted. On Windows quoting for the MSVCRT argv
/// parser happens here, since it is a property of the platform's process launch
/// rather than of the command being run; on POSIX execve takes the array
/// directly and no quoting exists to get wrong.
///
/// The implementation is a pimpl so that this header stays free of <windows.h>
/// and <unistd.h> alike: exactly one of child_process_win32.cpp and
/// child_process_posix.cpp is compiled.
class ChildProcessTransport final : public ITransport {
public:
    /// Launches `argv[0]` with the remaining entries as its arguments.
    ///
    /// `argv[0]` is executed as given rather than searched for on PATH, so
    /// callers pass a resolved path. `env` entries are merged over the parent's
    /// environment rather than replacing it, so the child keeps PATH and
    /// friends; see mergeEnvironment for how names are matched.
    ///
    /// Throws KernelError if the process or its pipes could not be created.
    /// This includes a failed exec on POSIX, which is reported synchronously
    /// through a close-on-exec status pipe rather than surfacing later as a
    /// child that mysteriously exits with 127.
    explicit ChildProcessTransport(const std::vector<std::string> &argv,
                                   const std::vector<EnvOverride> &env = {});

    ~ChildProcessTransport() override;

    void send(std::string_view bytes) override;
    std::string receive(std::chrono::milliseconds timeout) override;
    bool alive() const override;
    void kill() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mx::detail
