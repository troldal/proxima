#pragma once

#include <proxima/config.hpp>
#include <proxima/expr.hpp>
#include <proxima/reply.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

namespace proxima {

namespace detail {
class MaximaSession;
}

/// A persistent Maxima session.
///
/// The session is started on construction and shut down on destruction, so a
/// single Kernel serves many queries without paying process startup each time.
///
/// The operations in proxima/ops.hpp are the intended way in; this is the layer
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

    /// Moves the Maxima session. The kernel moved from has none left: it can
    /// be destroyed or assigned to, and any other call on it throws
    /// proxima::KernelError. So does an proxima::Context still pointing at it.
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
    /// proxima::KernelError.
    ///
    /// **Discards the reply cache.** This entry point can evaluate anything,
    /// including statements that change Maxima's state — an assignment, a new
    /// assumption, a redefined function — and there is no way to tell from the
    /// text which. Assuming the worst is the only safe default: a cache that
    /// returns a stale answer is a correctness bug, and a needlessly emptied
    /// cache is merely slower. Use eval_pure for anything known to be a
    /// question rather than an instruction.
    ///
    /// **Stops Config::cache_directory for this kernel,** for the same reason: a
    /// persistent answer is keyed on the state this kernel has recorded, and an
    /// eval may have changed Maxima in a way nothing recorded. It stays off
    /// until restart(); persistence_active() says which way things stand.
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
    /// any assumption added or dropped through proxima::Context.
    Reply eval_pure(std::string_view expression);
    Reply eval_pure(const Expr &form);

    /// Evaluates a statement that changes Maxima's state in a way this
    /// kernel's replay journal accounts for.
    ///
    /// The caller promises that the change is either being recorded through
    /// remember(), or is undoing something that was. On that promise the
    /// kernel's state stays fully described by its journal, which is what keeps
    /// Config::cache_directory usable — a persistent entry is keyed on that
    /// state, so an unrecorded change would make the key a lie.
    ///
    /// proxima::Context is the intended caller; there is rarely a reason to use this
    /// directly. Use eval() for anything else, which assumes the worst.
    Reply eval_tracked(std::string_view statement);
    Reply eval_tracked(const Expr &form);

    /// eval, with the reply read into an expression: the way to call a Maxima
    /// function this library has not wrapped without reading s-expressions.
    /// `kernel.eval_expr("gcd(12, 18)")` is 6.
    ///
    /// A Maxima error is the Failure, carrying Maxima's message.
    ///
    /// **This is a statement, not a query.** Like eval, every call discards the
    /// reply cache and switches Config::cache_directory off for this kernel
    /// until restart(), because the text might have changed anything. For a
    /// question known to change nothing — `gcd(12, 18)` is one — read an
    /// eval_pure reply with proxima::to_expr instead, which keeps both.
    std::expected<Expr, Failure> eval_expr(std::string_view expression);
    std::expected<Expr, Failure> eval_expr(const Expr &form);

    /// Forgets every cached reply. Rarely needed directly — state changes made
    /// through this library already do it — but the escape hatch if Maxima has
    /// been changed some other way.
    void invalidate_cache();

    /// Hits, misses and current size, for tuning and for tests. Does not
    /// wait for a call in progress.
    struct CacheStats {
        std::size_t hits = 0;
        std::size_t misses = 0;
        std::size_t entries = 0;
        /// Answers that came from Config::cache_directory rather than from
        /// Maxima — that is, from a previous run or another process.
        std::size_t persistent_hits = 0;
    };
    CacheStats cache_stats() const;

    /// Records a statement to replay if the kernel has to be restarted, and
    /// returns a handle for removing it again.
    ///
    /// Maxima is a separate process holding mutable state — assumptions,
    /// declarations, bindings — that a restart would otherwise silently lose,
    /// making later results quietly wrong rather than obviously broken. Scoped
    /// state such as proxima::Context registers itself here; there is rarely a
    /// reason to call this directly.
    std::uint64_t remember(std::string_view statement);
    std::uint64_t remember(const Expr &form);

    /// Stops replaying the statement `handle` names.
    void forget(std::uint64_t handle);

    /// Changes the per-call deadline for this kernel — including for a call
    /// already waiting on Maxima, which is held to the new deadline within one
    /// poll. Does not wait for that call. Config::startup_timeout, which governs
    /// launching and restarting, is unaffected.
    void set_timeout(std::chrono::milliseconds timeout);

    /// True while answers are read from and written to Config::cache_directory.
    ///
    /// False when no directory was configured, and also after a raw eval():
    /// that call may have changed Maxima's state in a way nothing recorded,
    /// and a persistent answer is keyed on the recorded state, so persistence
    /// stops rather than file answers under conditions that may not hold.
    /// restart() brings it back.
    bool persistence_active() const;

    /// Replaces Maxima with a fresh process and replays what this kernel
    /// remembers — every proxima::Context's assumptions and declarations — so the
    /// session is exactly what that record describes.
    ///
    /// Anything else is lost, which is the point: bindings and definitions made
    /// through a raw eval() are discarded, and with them the reason persistence
    /// had stopped, so it resumes. Costs a Maxima startup.
    void restart();

private:
    friend class Context;

    /// The session, or KernelError for a kernel that has been moved from.
    detail::MaximaSession &session() const;

    std::unique_ptr<detail::MaximaSession> session_;

    /// Alive exactly as long as this kernel's session, and moved along with
    /// it. A Context keeps a weak reference, so one that outlives the kernel —
    /// a static Context at exit, after shared_kernel() has been destroyed —
    /// can tell, instead of calling into a destroyed object. Declared after
    /// session_, so it expires first.
    std::shared_ptr<const int> lifetime_ = std::make_shared<const int>(0);
};

/// A reply read into an expression: its value when `ok`, and a Failure
/// carrying the reason when not. How every operation in proxima/ops.hpp reads its
/// reply, and how to read one from eval_pure or eval_tracked.
///
/// Throws proxima::ParseError if the value is not a Maxima term. No reply from a
/// kernel should be one: it would mean the protocol itself had failed.
std::expected<Expr, Failure> to_expr(const Reply &reply);

} // namespace proxima
