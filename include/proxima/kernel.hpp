#pragma once

#include <proxima/assumptions.hpp>
#include <proxima/config.hpp>
#include <proxima/expr.hpp>
#include <proxima/result.hpp>

#include <fxt/utils/Unit.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

namespace proxima {

class Kernel;
class Query;

namespace detail {
class MaximaSession;

/// A question's answer as Maxima's wire text, before it is read into an
/// Expr. For the library's own tests of the protocol; see kernel.cpp.
result<std::string> ask_wire(Kernel &kernel, const Query &query,
                             const Assumptions &assumptions);
} // namespace detail

/// A question for Maxima: something that changes nothing, so its answer can
/// be cached and kept between runs.
///
/// The type is the promise. Kernel::ask takes only a Query, and a Query is
/// only ever an expression or text the caller is asking about — so there is
/// no "evaluate, and promise it is pure" to get wrong, and no way to write a
/// statement where a question was meant without saying Statement.
class Query {
public:
    /// An expression, sent as structure: its internal s-expression. What
    /// every operation in proxima/ops.hpp asks.
    static Query form(Expr expr) { return Query(std::move(expr)); }

    /// Maxima source text, for anything an Expr cannot say:
    /// `Query::text("gcd(12, 18)")`. Maxima parses it, inside the error
    /// trap, so malformed text is an ordinary failure.
    static Query text(std::string source) { return Query(std::move(source)); }

private:
    friend class Kernel;
    friend result<std::string> detail::ask_wire(Kernel &, const Query &,
                                                const Assumptions &);
    explicit Query(std::variant<Expr, std::string> content) : content_(std::move(content)) {}
    std::variant<Expr, std::string> content_;
};

/// Something for Maxima to do: an assignment, a definition, a change to how
/// it simplifies. Kernel::tell is the only thing that takes one, and the only
/// thing that can leave Maxima different from how it found it.
class Statement {
public:
    static Statement form(Expr expr) { return Statement(std::move(expr)); }
    static Statement text(std::string source) { return Statement(std::move(source)); }

private:
    friend class Kernel;
    explicit Statement(std::variant<Expr, std::string> content)
        : content_(std::move(content)) {}
    std::variant<Expr, std::string> content_;
};

/// A persistent Maxima session.
///
/// The session is started on construction and shut down on destruction, so a
/// single Kernel serves many queries without paying process startup each time.
///
/// The operations in proxima/ops.hpp are the intended way in; this is the layer
/// beneath them, with two verbs. ask answers a Query under a set of
/// Assumptions, and is cached; tell carries out a Statement, and is not. Both
/// report a Maxima error as the Failure — Cause::MaximaError, or
/// Cause::NeedsAssumption when Maxima wanted a fact, or Cause::Inconsistent
/// when the assumptions contradict one another — because failing to
/// integrate something is an ordinary outcome. Only infrastructure failures
/// throw: see proxima::KernelError.
///
/// Thread-safe: calls are serialised, so concurrent callers take turns rather
/// than interleaving requests on one pipe. That makes a Kernel safe to share,
/// not fast to share — Maxima itself is one process doing one thing at a time.
/// For genuine parallelism, give each thread its own Kernel.
///
/// Survives its own death. If Maxima hangs or exits, the failing call reports
/// it and the kernel is restarted behind the scenes, so the next call starts
/// from a working session.
///
/// ## Assumptions, and why they are not state here
///
/// The assumptions travel with each question. The kernel gives each distinct
/// set a Maxima context of its own the first time it is used, keeps the most
/// recent sixteen, and switches between them as questions arrive; an answer
/// is cached under the question *and* its assumptions. So nothing a caller
/// does to one question can change the answer to another, on any thread,
/// and a restart loses nothing that matters: the contexts are made again when
/// next needed.
class Kernel {
public:
    explicit Kernel(Config config = {});
    ~Kernel();

    /// Moves the Maxima session. The kernel moved from has none left: it can
    /// be destroyed or assigned to, and any other call on it throws
    /// proxima::KernelError.
    Kernel(Kernel &&) noexcept;
    Kernel &operator=(Kernel &&) noexcept;

    Kernel(const Kernel &) = delete;
    Kernel &operator=(const Kernel &) = delete;

    /// Answers a question under `assumptions`, from the reply cache when it
    /// has been asked before under the same ones, and from
    /// Config::cache_directory when a previous run asked it.
    result<Expr> ask(const Query &query, const Assumptions &assumptions = {});

    /// Carries out a statement: `Statement::text("a: 7")`.
    ///
    /// **Empties the reply cache, and stops Config::cache_directory for this
    /// kernel until restart().** Nothing in a statement says what it changed,
    /// and a cache that returns a stale answer is a correctness bug, where an
    /// emptied one is merely slower. persistence_active() says which way
    /// things stand.
    ///
    /// Runs outside every assumption context, so what it does is in force
    /// under every set of assumptions — and is lost at restart(), which is the
    /// way back to a Maxima the caches can describe.
    result<fxt::unit> tell(const Statement &statement);

    /// Forgets every cached reply. Rarely needed directly — tell already does
    /// it — but the escape hatch if Maxima has been changed some other way.
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

    /// Changes the per-call deadline for this kernel — including for a call
    /// already waiting on Maxima, which is held to the new deadline within one
    /// poll. Does not wait for that call. Config::startup_timeout, which governs
    /// launching and restarting, is unaffected.
    void set_timeout(std::chrono::milliseconds timeout);

    /// True while answers are read from and written to Config::cache_directory:
    /// a directory was configured, and no tell() has changed Maxima since the
    /// kernel started or was restarted.
    bool persistence_active() const;

    /// Replaces Maxima with a fresh process. Everything a tell() did is lost,
    /// and with it the reason persistence had stopped, so it resumes. Costs a
    /// Maxima startup.
    void restart();

private:
    friend result<std::string> detail::ask_wire(Kernel &, const Query &,
                                                const Assumptions &);

    /// The session, or KernelError for a kernel that has been moved from.
    detail::MaximaSession &session() const;

    std::unique_ptr<detail::MaximaSession> session_;
};

/// The process-wide kernel, started on first use and shut down at exit.
///
/// Convenient rather than obligatory: every operation takes an Env naming the
/// kernel to use, defaulting to this one. Safe to use from several threads,
/// like any Kernel: starting it on first use is thread-safe, and calls on it
/// take turns.
Kernel &shared_kernel();

/// Where an operation runs: which kernel, under which assumptions.
///
/// Every operation in proxima/ops.hpp takes one as its last parameter,
/// defaulting to shared_kernel() and no assumptions, and it converts from
/// either half or both:
///
///     proxima::integrate(f, x);                         // shared kernel, nothing assumed
///     proxima::integrate(f, x, assuming(gt(n, 0)));     // shared kernel, n > 0
///     proxima::integrate(f, x, kernel);                 // this kernel, nothing assumed
///     proxima::integrate(f, x, {assuming(gt(n, 0)), kernel});
///
/// A value: the environment is passed in, never looked up, so what an
/// operation means is decided at the call.
class Env {
public:
    Env() = default;
    Env(Kernel &kernel) : kernel_(&kernel) {}                                // NOLINT: implicit on purpose
    Env(Assumptions assumptions) : assumptions_(std::move(assumptions)) {}   // NOLINT: implicit on purpose
    Env(Assumptions assumptions, Kernel &kernel)
        : kernel_(&kernel), assumptions_(std::move(assumptions)) {}

    /// The kernel: the one given, or shared_kernel().
    Kernel &kernel() const { return kernel_ != nullptr ? *kernel_ : shared_kernel(); }

    const Assumptions &assumptions() const { return assumptions_; }

    /// The same kernel under these assumptions and `more`.
    [[nodiscard]] Env with(const Assumptions &more) const {
        Env wider = *this;
        wider.assumptions_ = assumptions_.with(more);
        return wider;
    }

private:
    Kernel *kernel_ = nullptr;
    Assumptions assumptions_;
};

} // namespace proxima
