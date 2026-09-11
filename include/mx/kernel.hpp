#pragma once

#include <mx/config.hpp>
#include <mx/reply.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace mx {

class Expr;

namespace detail {
class MaximaSession;
}

/// A persistent Maxima session.
///
/// The session is started on construction and shut down on destruction, so a
/// single Kernel serves many queries without paying process startup each time.
///
/// The operations in mx/ops.hpp are the intended way in; this is the layer
/// beneath them. Each entry point comes in two forms. One takes an Expr, which
/// travels to Maxima as structure — its internal s-expression — and is the
/// form the operations use. The other takes Maxima source text, for anything
/// an Expr cannot say; Maxima parses it itself, inside the same error trap,
/// so a malformed string is an ordinary failure rather than a stall. Both
/// hand back the text of Maxima's internal reply.
///
/// Thread-safe: calls are serialised, so concurrent callers take turns rather
/// than interleaving requests on one pipe. That makes a Kernel safe to share,
/// not fast to share — Maxima itself is one process doing one thing at a time.
/// For genuine parallelism, give each thread its own Kernel.
///
/// Survives its own death. If Maxima hangs or exits, the failing call reports
/// it and the kernel is restarted behind the scenes with its remembered state
/// replayed, so the next call starts from a working session.
class Kernel {
public:
    explicit Kernel(Config config = {});
    ~Kernel();

    Kernel(Kernel &&) noexcept;
    Kernel &operator=(Kernel &&) noexcept;

    Kernel(const Kernel &) = delete;
    Kernel &operator=(const Kernel &) = delete;

    /// Evaluates Maxima source text — `integrate(x^2, x)` — or, in the Expr
    /// form, an expression sent as structure.
    ///
    /// A Maxima error comes back as a Reply with `ok == false` and a reason,
    /// because failing to integrate something is an ordinary outcome. That
    /// includes text Maxima cannot even parse: it is read inside the error
    /// trap, so `eval("(1")` fails with a message rather than waiting out
    /// Config::timeout. Only infrastructure failures throw: see
    /// mx::KernelError.
    ///
    /// **Discards the reply cache.** This entry point can evaluate anything,
    /// including statements that change Maxima's state — an assignment, a new
    /// assumption, a redefined function — and there is no way to tell from the
    /// text which. Assuming the worst is the only safe default: a cache that
    /// returns a stale answer is a correctness bug, and a needlessly emptied
    /// cache is merely slower. Use evalPure for anything known to be a
    /// question rather than an instruction.
    Reply eval(std::string_view expression);
    Reply eval(const Expr &form);

    /// Evaluates a *pure* expression, consulting and filling the reply cache.
    ///
    /// The caller promises `expression` only asks a question: it must not
    /// assign, assume, declare, define, or otherwise leave Maxima different
    /// from how it found it. Break that promise and later callers will be
    /// handed answers computed under conditions that no longer hold.
    ///
    /// Cached answers are still answers to *this* kernel's current state. The
    /// cache is discarded whenever that state might have changed: any eval(),
    /// any assumption added or dropped through mx::Context.
    Reply evalPure(std::string_view expression);
    Reply evalPure(const Expr &form);

    /// Evaluates a statement that changes Maxima's state in a way this
    /// kernel's replay journal accounts for.
    ///
    /// The caller promises that the change is either being recorded through
    /// remember(), or is undoing something that was. On that promise the
    /// kernel's state stays fully described by its journal, which is what keeps
    /// Config::cacheDirectory usable — a persistent entry is keyed on that
    /// state, so an unrecorded change would make the key a lie.
    ///
    /// mx::Context is the intended caller; there is rarely a reason to use this
    /// directly. Use eval() for anything else, which assumes the worst.
    Reply evalTracked(std::string_view statement);
    Reply evalTracked(const Expr &form);

    /// Forgets every cached reply. Rarely needed directly — state changes made
    /// through this library already do it — but the escape hatch if Maxima has
    /// been changed some other way.
    void invalidateCache();

    /// Hits, misses and current size, for tuning and for tests.
    struct CacheStats {
        std::size_t hits = 0;
        std::size_t misses = 0;
        std::size_t entries = 0;
        /// Answers that came from Config::cacheDirectory rather than from
        /// Maxima — that is, from a previous run or another process.
        std::size_t persistentHits = 0;
    };
    CacheStats cacheStats() const;

    /// Records a statement to replay if the kernel has to be restarted, and
    /// returns a handle for removing it again.
    ///
    /// Maxima is a separate process holding mutable state — assumptions,
    /// declarations, bindings — that a restart would otherwise silently lose,
    /// making later results quietly wrong rather than obviously broken. Scoped
    /// state such as mx::Context registers itself here; there is rarely a
    /// reason to call this directly.
    std::uint64_t remember(std::string statement);
    std::uint64_t remember(const Expr &form);

    /// Stops replaying the statement `handle` names.
    void forget(std::uint64_t handle);

    /// Changes the per-call deadline for this kernel. Config::startupTimeout,
    /// which governs launching and restarting, is unaffected.
    void setTimeout(std::chrono::milliseconds timeout);

private:
    std::unique_ptr<detail::MaximaSession> session_;
};

} // namespace mx
