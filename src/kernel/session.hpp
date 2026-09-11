#pragma once

// Internal header, but note what is *not* here any more: as of PLAN.md step 4
// this class holds no handles and includes no platform headers. It speaks the
// Maxima protocol over an ITransport and nothing else.

#include "transport/itransport.hpp"

#include <mx/config.hpp>

#include <memory>
#include <string>
#include <vector>

namespace mx::detail {

/// Drives a persistent Maxima session.
///
/// Maxima is reached directly through its underlying SBCL Lisp image, bypassing
/// maxima.bat/cmd.exe entirely. That avoids spawning a fresh Maxima process (and
/// a flashing console window) per query, and avoids the batch-mode echo/marker
/// parsing hacks: Maxima's *prompt-prefix*/*prompt-suffix* Lisp hooks (see
/// doc/implementation/external-interface.txt) let every prompt be wrapped in
/// unambiguous, distinctive markers so results can be split out reliably.
///
/// The prompt-marker scheme is an interim measure. Step 6 replaces it with
/// correlation-ID framing around errcatch, which makes a desynchronised stream
/// detectable rather than silently off by one reply.
class MaximaSession {
public:
    /// Discovers Maxima under `config.maximaRoot` and launches it.
    explicit MaximaSession(Config config);

    /// Drives an already-constructed transport. The handshake still runs, so a
    /// scripted transport must answer it. For tests.
    MaximaSession(std::unique_ptr<ITransport> transport, Config config);

    ~MaximaSession();

    MaximaSession(const MaximaSession &) = delete;
    MaximaSession &operator=(const MaximaSession &) = delete;

    /// Sends one Maxima statement (must end in `;` or `$`) and returns the text
    /// of its result line (e.g. "2*x*sin(x)+cos(x)*(2-x**2)"), or an empty
    /// string if the statement produced no output (terminated in `$`).
    ///
    /// Throws KernelError if the session died or did not answer in time.
    std::string evaluate(const std::string &statement);

    /// Markers wrapped around every Maxima prompt. Public so tests can script a
    /// transport that speaks the same protocol.
    static constexpr const char *kPromptPrefix = "@MAXIMA_PROMPT_BEGIN@";
    static constexpr const char *kPromptSuffix = "@MAXIMA_PROMPT_END@";

    /// Builds the argv used to launch Maxima's SBCL image for `config`.
    /// Exposed for testing; performs filesystem lookups but starts nothing.
    static std::vector<std::string> launchCommand(const Config &config);

private:
    void handshake();
    void writeLine(const std::string &line);
    std::string readUntilPrompt();

    Config config_;
    std::unique_ptr<ITransport> transport_;
};

} // namespace mx::detail
