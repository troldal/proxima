#pragma once

#include "transport/itransport.hpp"
#include "transport/process_env.hpp"

#include <memory>
#include <string>
#include <vector>

namespace proxima::detail {

/// Runs a child process with its stdin and stdout (and stderr) bound to pipes.
///
/// Generic: it is handed an argv and moves bytes. Nothing here knows what
/// Maxima or SBCL are, which is what keeps the Maxima-specific launch details
/// in MaximaSession where they can be tested against FakeTransport.
///
/// Implemented over Boost.Process v2 and Boost.Asio, one file for every
/// platform. Arguments are supplied unquoted: quoting for the Windows argv
/// parser is Boost.Process's business, and POSIX needs none.
///
/// The implementation is a pimpl, so this header stays free of Asio and of
/// every platform header, and so does everything that includes it.
class ChildProcessTransport final : public ITransport {
public:
    /// Launches `argv[0]` with the remaining entries as its arguments.
    ///
    /// Every string — the executable, the arguments, the environment names
    /// and values — is UTF-8 on every platform, as it is everywhere inside the
    /// library (see util/utf8.hpp). Not the ANSI code page: a path taken from
    /// std::filesystem::path::string() on Windows is the wrong encoding here.
    ///
    /// `argv[0]` is executed as given rather than searched for on PATH, so
    /// callers pass a resolved path. `env` entries are merged over the parent's
    /// environment rather than replacing it, so the child keeps PATH and
    /// friends; see merge_environment for how names are matched.
    ///
    /// Throws KernelError if the process or its pipes could not be created,
    /// including an executable that does not exist. That is reported
    /// synchronously on every platform, rather than surfacing later as a child
    /// that mysteriously exits.
    explicit ChildProcessTransport(const std::vector<std::string> &argv,
                                   const std::vector<EnvOverride> &env = {});

    ~ChildProcessTransport() override;

    void send(std::string_view bytes) override;
    std::string receive(std::chrono::milliseconds timeout) override;
    bool alive() const override;
    void kill() override;
    void terminate() override;

private:
    /// Closes the child's input, waits up to `grace` for it to exit, then
    /// terminates it if it has not.
    void stop(std::chrono::milliseconds grace);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace proxima::detail
