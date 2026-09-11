#pragma once

// Internal header. Note what is *not* here: this class holds no handles and
// includes no platform headers. It speaks the Maxima protocol over an
// ITransport and nothing else.

#include "kernel/discovery.hpp"
#include "transport/itransport.hpp"
#include "transport/process_env.hpp"

#include <mx/config.hpp>
#include <mx/reply.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mx::detail {

/// Drives a persistent Maxima session.
///
/// Maxima is reached directly through its underlying SBCL Lisp image, bypassing
/// maxima.bat//usr/bin/maxima entirely. That avoids spawning a fresh Maxima
/// process (and, on Windows, a flashing console window) per query.
///
/// ## The protocol
///
/// A Lisp helper installed at startup wraps every reply in delimiters carrying
/// the request's own id:
///
///     @@B<id>@@<ok>@@S<id>@@<value>@@S<id>@@<reason>@@E<id>@@
///
/// and each request is sent as
///
///     cppsend(<id>, errcatch(ratdisrep(<expression>)))$
///
/// Four things fall out of that shape, each of which the prototype's
/// prompt-marker scheme got wrong:
///
/// - **The id makes desynchronisation detectable.** A reply is only accepted
///   for the request that asked for it; a stale or duplicated frame is skipped
///   rather than silently returned as the answer to the wrong question.
/// - **errcatch turns errors into values.** It yields `[]` on failure and
///   `[result]` on success, so Maxima never drops into an error prompt that
///   leaves the stream off by one. Success and failure are read from the frame
///   rather than guessed at from the shape of some text.
/// - **The value is Maxima's internal s-expression, not its display output.**
///   `(%oN)` text is a display format: it is ambiguous, it line-wraps, and it
///   loses exact rationals. The internal form has no precedence to re-derive
///   and keeps `((RAT SIMP) 1 3)` as a rational. `ratdisrep` prevents canonical
///   rational (`MRAT`) forms coming back in place of general ones.
/// - **Errors no longer leak into the stream.** With `errormsg:false` Maxima
///   stops printing them, and the helper renders the message into the frame, so
///   everything between frames is noise that can simply be discarded.
class MaximaSession {
public:
    /// Produces a transport, and can be asked again after one dies. Holding a
    /// factory rather than a transport is what makes restarting possible.
    using TransportFactory = std::function<std::unique_ptr<ITransport>()>;

    /// Discovers Maxima under `config.maximaRoot` and launches it.
    explicit MaximaSession(Config config);

    /// Drives transports from `factory`, which is called again on restart.
    /// For tests.
    MaximaSession(TransportFactory factory, Config config);

    /// Drives one already-constructed transport, with no way to make another,
    /// so a death is final. For tests.
    MaximaSession(std::unique_ptr<ITransport> transport, Config config);

    ~MaximaSession();

    MaximaSession(const MaximaSession &) = delete;
    MaximaSession &operator=(const MaximaSession &) = delete;

    /// Evaluates one Maxima *expression* — `integrate(x^2, x)`, not
    /// `integrate(x^2, x);` — and returns its result.
    ///
    /// Note the change from the prototype: no terminator, because the
    /// expression is substituted into a wrapper that supplies its own. A
    /// Maxima error is reported through Reply::ok rather than thrown.
    ///
    /// Throws TimeoutError if Maxima did not answer within Config::timeout, or
    /// KernelError if the session died or answered something unintelligible. In
    /// either case the kernel is restarted and its remembered state replayed
    /// first, so only this call is lost.
    ///
    /// Serialised: concurrent callers take turns rather than interleaving
    /// requests on one pipe.
    Reply eval(std::string_view expression);

    /// Records a statement to replay after a restart, and returns a handle for
    /// removing it again.
    ///
    /// The kernel is a separate process holding mutable state — assumptions,
    /// declarations, bindings — that a restart would otherwise silently lose,
    /// leaving later results quietly wrong rather than obviously broken. Scoped
    /// state such as mx::Context registers itself here.
    std::uint64_t remember(std::string statement);

    /// Stops replaying the statement `handle` names.
    void forget(std::uint64_t handle);

    /// Changes the per-call deadline. Does not affect Config::startupTimeout.
    void setTimeout(std::chrono::milliseconds timeout);

    /// Builds the argv used to launch Maxima's SBCL image for `install`.
    /// Exposed for testing; touches no filesystem and starts nothing.
    static std::vector<std::string> launchCommand(const MaximaInstall &install);

    /// Builds the environment overrides layered over the parent's environment.
    /// Exposed for testing; may create Config::userDir but starts nothing.
    static std::vector<EnvOverride> launchEnvironment(const MaximaInstall &install,
                                                      const Config &config);

    /// The statements sent once at startup to make the session machine-readable
    /// and deterministic. Exposed so tests can script a transport that expects
    /// exactly these.
    static std::vector<std::string> setupStatements();

    /// Wraps `expression` in the framed, error-trapping call sent to Maxima.
    /// Exposed for testing.
    static std::string requestFor(std::uint64_t id, std::string_view expression);

    /// Frame delimiters for request `id`. Exposed so tests can script replies
    /// in the same shape Maxima produces.
    static std::string frameBegin(std::uint64_t id);
    static std::string frameSeparator(std::uint64_t id);
    static std::string frameEnd(std::uint64_t id);

private:
    struct JournalEntry {
        std::uint64_t handle;
        std::string statement;
    };

    /// The body of eval, with the lock already held.
    Reply evalLocked(std::string_view expression,
                     std::chrono::milliseconds timeout);

    /// Discards the dead transport, builds another, and restores the session:
    /// handshake, then every remembered statement in the order it was made.
    void recover();

    void handshake();
    void writeLine(std::string_view line);

    /// Reads until the frame belonging to `id` is complete, discarding
    /// everything before it: banners, prompts, and any stale frame left over
    /// from an earlier request.
    Reply readFrame(std::uint64_t id, std::chrono::milliseconds timeout);

    mutable std::mutex mutex_;
    Config config_;
    TransportFactory factory_;
    std::unique_ptr<ITransport> transport_;
    std::uint64_t nextRequestId_ = 0;

    std::vector<JournalEntry> journal_;
    std::uint64_t nextJournalHandle_ = 0;

    /// Set while recovering, so that a failure during the handshake or replay
    /// does not set off another recovery inside the first.
    bool recovering_ = false;
};

} // namespace mx::detail
