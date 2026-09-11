#pragma once

// Internal header. Everything Win32 lives on this side of the wall so that
// <windows.h> never reaches include/mx. PLAN.md step 4 replaces the guts of
// this class with an ITransport implementation.

// Guarded: MinGW's libstdc++ already defines NOMINMAX in os_defines.h, and
// unlike the prototype this header is not guaranteed to be included before
// any standard header.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <mx/config.hpp>

#include <string>

namespace mx::detail {

/// Drives a persistent Maxima session directly through its underlying SBCL
/// Lisp image (bypassing maxima.bat/cmd.exe entirely), communicating over
/// anonymous pipes. This avoids spawning a fresh Maxima process (and a
/// flashing console window) per query, and avoids the batch-mode echo/marker
/// parsing hacks: Maxima's *prompt-prefix*/*prompt-suffix* Lisp hooks (see
/// doc/implementation/external-interface.txt) let us wrap every prompt in
/// unambiguous, distinctive markers so results can be split out reliably.
class MaximaSession {
public:
    explicit MaximaSession(Config config);
    ~MaximaSession();

    MaximaSession(const MaximaSession &) = delete;
    MaximaSession &operator=(const MaximaSession &) = delete;

    /// Sends one Maxima statement (must end in `;` or `$`) and returns the
    /// text of its result line (e.g. "2*x*sin(x)+cos(x)*(2-x**2)"), or an
    /// empty string if the statement produced no output (terminated in `$`).
    std::string evaluate(const std::string &statement);

private:
    static constexpr const char *kPromptPrefix = "@MAXIMA_PROMPT_BEGIN@";
    static constexpr const char *kPromptSuffix = "@MAXIMA_PROMPT_END@";

    void start();
    void stop();
    void writeLine(const std::string &line);
    std::string readUntilPrompt();

    Config config_;
    HANDLE stdinWrite_ = nullptr;
    HANDLE stdoutRead_ = nullptr;
    PROCESS_INFORMATION procInfo_{};
};

} // namespace mx::detail
