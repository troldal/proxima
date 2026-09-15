#pragma once

#include <chrono>
#include <string>
#include <string_view>

namespace proxima::detail {

/// A byte pipe to a child process.
///
/// Deliberately knows nothing about Maxima: no prompts, no markers, no
/// statements. Framing and protocol live one layer up, in MaximaSession.
/// That split is what lets FakeTransport exercise the protocol with no
/// installation present, and what let the process handling underneath be
/// replaced wholesale — hand-written Win32 and POSIX code, then Boost.Process —
/// without touching the session.
class ITransport {
public:
    virtual ~ITransport() = default;

    ITransport() = default;
    ITransport(const ITransport &) = delete;
    ITransport &operator=(const ITransport &) = delete;

    /// Writes bytes to the child's standard input.
    virtual void send(std::string_view bytes) = 0;

    /// Reads whatever bytes are available, waiting up to `timeout` for the
    /// first of them.
    ///
    /// Returns a short read rather than waiting to fill anything — the caller
    /// is expected to accumulate until it recognises a frame. An empty return
    /// means either the timeout elapsed with nothing to read or the stream has
    /// closed; call alive() to tell those apart.
    virtual std::string receive(std::chrono::milliseconds timeout) = 0;

    /// False once the child has exited or its output stream has closed.
    virtual bool alive() const = 0;

    /// Ends a child that has been asked to leave, and releases its handles.
    /// Closes its input and gives it a grace period to exit on its own —
    /// to flush, to finish cleanly — before terminating it. Idempotent.
    virtual void kill() = 0;

    /// Ends the child at once, without the grace period, and releases its
    /// handles. For a child that was never asked to leave and will not: one
    /// still busy with a computation that timed out. Idempotent.
    virtual void terminate() = 0;
};

} // namespace proxima::detail
