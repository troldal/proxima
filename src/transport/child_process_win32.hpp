#pragma once

// Internal header: the only place <windows.h> is pulled in.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "transport/itransport.hpp"

#include <string>
#include <utility>
#include <vector>

namespace mx::detail {

/// Runs a child process with its stdin and stdout (and stderr) bound to
/// anonymous pipes.
///
/// Generic: it is handed an argv and moves bytes. Nothing here knows what
/// Maxima or SBCL are, which is what keeps the Maxima-specific launch details
/// in MaximaSession where they can be tested against FakeTransport.
///
/// Arguments are supplied unquoted; quoting for the MSVCRT argv parser is this
/// class's job, since it is a property of the platform's process launch rather
/// than of the command being run.
class ChildProcessTransport final : public ITransport {
public:
    /// A name/value pair layered over the inherited environment.
    using EnvOverride = std::pair<std::string, std::string>;

    /// Launches `argv[0]` with the remaining entries as its arguments.
    ///
    /// `env` entries are merged over the parent's environment — matched
    /// case-insensitively, as Windows treats variable names — rather than
    /// replacing it, so the child keeps PATH and friends.
    ///
    /// Throws KernelError if the process or its pipes could not be created.
    explicit ChildProcessTransport(const std::vector<std::string> &argv,
                                   const std::vector<EnvOverride> &env = {});

    ~ChildProcessTransport() override;

    void send(std::string_view bytes) override;
    std::string receive(std::chrono::milliseconds timeout) override;
    bool alive() const override;
    void kill() override;

private:
    HANDLE stdinWrite_ = nullptr;
    HANDLE stdoutRead_ = nullptr;
    PROCESS_INFORMATION procInfo_{};
    bool closed_ = false;
};

/// Quotes a single argument for a Win32 CreateProcess command line, following
/// the escaping rules understood by the standard MSVCRT argv parser (doubling
/// backslashes that precede a quote, escaping embedded quotes).
///
/// Unlike passing arguments through cmd.exe/_popen, CreateProcess takes one
/// command-line string and hands it straight to the child's argv parser, so
/// this is well-defined and avoids the batch-file quoting bugs previously hit
/// with `;` and nested quotes.
///
/// Exposed for testing; not part of the public API.
std::string quoteArg(const std::string &arg);

/// Merges `overrides` over the current process environment and returns a
/// CreateProcess environment block: "NAME=VALUE\0...\0\0".
///
/// Names match case-insensitively, since that is how Windows compares them —
/// overriding "maxima_prefix" must replace an inherited "MAXIMA_PREFIX" rather
/// than sit beside it, which the child would see as undefined behaviour.
///
/// Exposed for testing; not part of the public API.
std::vector<char>
buildEnvironmentBlock(const std::vector<ChildProcessTransport::EnvOverride> &overrides);

} // namespace mx::detail
