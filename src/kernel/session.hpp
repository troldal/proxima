#pragma once

// Internal header. Note what is *not* here: this class holds no handles and
// includes no platform headers. It speaks the Maxima protocol over an
// ITransport and nothing else.

#include "kernel/cache.hpp"
#include "kernel/persistent_cache.hpp"
#include "kernel/discovery.hpp"
#include "transport/itransport.hpp"
#include "transport/process_env.hpp"

#include <proxima/config.hpp>
#include "kernel/reply.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace proxima::detail {

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
///     @@B<tag>@@<ok>@@S<tag>@@<value>@@S<tag>@@<reason>@@E<tag>@@
///
/// where the tag is `<key>-<id>`: the request's id, behind a key drawn at
/// random for each session. The id alone was enough to keep one reply from
/// being mistaken for another, but not to keep a *value* from ending its own
/// frame — a Maxima string containing `@@E7@@`, printed inside frame 7, did
/// exactly that, and ids are sequential and so easy to guess. The key is never
/// part of any value Maxima is asked to compute.
///
/// and each request is sent as
///
///     cppsend(<id>, errcatch(ratdisrep(<payload>)))$
///
/// where the payload is one of exactly two calls, each taking a single string
/// literal that this layer escaped itself:
///
///     cppread("((MPLUS) 1 $X)")        a Maxima internal form, read and evaluated
///     eval_string("integrate(x^2, x)") Maxima source, parsed and evaluated
///
/// Five things fall out of that shape:
///
/// - **The id makes desynchronisation detectable.** A reply is only accepted
///   for the request that asked for it; a stale or duplicated frame is skipped
///   rather than silently returned as the answer to the wrong question.
/// - **errcatch turns errors into values.** It yields `[]` on failure and
///   `[result]` on success, so Maxima never drops into an error prompt that
///   leaves the stream off by one. Success and failure are read from the frame
///   rather than guessed at from the shape of some text.
/// - **Nothing variable ever reaches Maxima's reader as syntax.** The only
///   text Maxima parses is the fixed wrapper plus a string literal. Reading
///   the string's *contents* happens inside errcatch, so a malformed
///   expression is an ordinary failure with a message. Before this, the
///   expression was spliced in raw, and a stray `$` failed in the reader —
///   before errcatch — which produced no frame at all and cost the caller the
///   full Config::timeout and a restart.
/// - **The value is Maxima's internal s-expression, not its display output.**
///   `(%oN)` text is a display format: it is ambiguous, it line-wraps, and it
///   loses exact rationals. The internal form has no precedence to re-derive
///   and keeps `((RAT SIMP) 1 3)` as a rational. `ratdisrep` prevents canonical
///   rational (`MRAT`) forms coming back in place of general ones. And with
///   `cppread` the *outbound* direction is the same form, so an expression
///   goes out as structure and comes back as structure: the infix printer is
///   no longer part of the protocol at all.
/// - **Errors no longer leak into the stream.** With `errormsg:false` Maxima
///   stops printing them, and the helper renders the message into the frame, so
///   everything between frames is noise that can simply be discarded.
/// The variable part of a request: a Maxima call taking one string literal.
///
/// Only the two factories can make one, and both escape the string they are
/// given, so there is no way to hand the session text that Maxima's reader
/// will see as syntax. That is the whole point of the type; see the protocol
/// notes above.
class Payload {
public:
    /// `cppread("<sexpr>")` — a Maxima internal form, to be read by the Lisp
    /// helper and evaluated. What to_maxima produces.
    static Payload form(std::string_view sexpr);

    /// `eval_string("<source>")` — Maxima source text, parsed and evaluated
    /// by Maxima's own parser. The escape hatch for anything an Expr cannot
    /// say.
    static Payload text(std::string_view source);

    /// The call, exactly as it is substituted into the request wrapper. Also
    /// the reply-cache key, since it is the whole question.
    const std::string &str() const { return call_; }

    bool operator==(const Payload &other) const = default;

private:
    explicit Payload(std::string call) : call_(std::move(call)) {}
    std::string call_;
};

/// A fresh frame key: sixteen hex digits from std::random_device.
std::string random_frame_key();

/// Creates `dir` accessible to its owner only, or accepts it if it already
/// exists as a directory — not a link — owned by this user and closed to
/// everyone else; otherwise throws KernelError. On Windows, only creates it.
void ensure_private_directory(const std::filesystem::path &dir);

/// The user directory Maxima is given when Config::user_dir is empty: a
/// per-user private directory under the system temporary directory, checked
/// with ensure_private_directory, since Maxima executes the maxima-init.mac it
/// finds there.
std::filesystem::path default_user_dir();

/// What eval_pure needs to answer under a set of assumptions: a key naming
/// the set, empty for none, and the statements that establish it in a fresh
/// Maxima context, each with the text a failure should quote.
struct Environment {
    struct Statement {
        Payload payload;
        std::string description;
    };

    std::string key;
    std::vector<Statement> statements;
};

class MaximaSession {
public:
    /// Produces a transport, and can be asked again after one dies. Holding a
    /// factory rather than a transport is what makes restarting possible.
    using TransportFactory = std::function<std::unique_ptr<ITransport>()>;

    /// Discovers Maxima under `config.maxima_root` and launches it.
    explicit MaximaSession(Config config);

    /// Drives transports from `factory`, which is called again on restart.
    /// For tests, which pass a fixed `frame_key` so that scripted replies can
    /// be written in advance.
    MaximaSession(TransportFactory factory, Config config,
                  std::string frame_key = random_frame_key());

    /// Drives one already-constructed transport, with no way to make another,
    /// so a death is final. For tests; `frame_key` as above.
    MaximaSession(std::unique_ptr<ITransport> transport, Config config,
                  std::string frame_key = random_frame_key());

    ~MaximaSession();

    MaximaSession(const MaximaSession &) = delete;
    MaximaSession &operator=(const MaximaSession &) = delete;

    /// Evaluates a statement: something that may change Maxima's state, and
    /// whose answer is therefore never cached. A Maxima error — including a
    /// malformed expression, which fails inside the payload's own reader — is
    /// reported through Reply::ok rather than thrown.
    ///
    /// Run in Maxima's `initial` context, outside any this session made for a
    /// set of assumptions. Nothing in the text says what it changed, so it
    /// empties the reply cache and stops persistence until restart().
    ///
    /// Throws TimeoutError if Maxima did not answer within Config::timeout, or
    /// KernelError if the session died or answered something unintelligible. In
    /// either case the kernel is restarted first, so only this call is lost.
    ///
    /// Serialised: concurrent callers take turns rather than interleaving
    /// requests on one pipe. Calls that only touch bookkeeping — cache_stats,
    /// set_timeout, invalidate_cache, persistence_active — do not take that
    /// turn, and never wait behind a computation.
    Reply eval(const Payload &payload);

    /// Evaluates a pure question under `environment`, consulting and filling
    /// the reply cache: the caller guarantees it changes nothing in Maxima.
    ///
    /// The assumptions live in a Maxima context of their own, made the first
    /// time they are asked under and kept, least recently used first, for as
    /// many as kMaxContexts sets; the session switches to it only when the
    /// last question was asked under different ones. Both caches key the
    /// answer on the environment's key as well as the question, so an answer
    /// is only ever read back under the assumptions that produced it.
    ///
    /// Fails, without asking, if the environment cannot be established: a
    /// Maxima error in one of its statements, or facts that contradict one
    /// another (a reason beginning kInconsistent).
    Reply eval_pure(const Payload &payload, const Environment &environment = {});

    /// Discards every cached reply.
    void invalidate_cache();

    struct CacheStats {
        std::size_t hits = 0;
        std::size_t misses = 0;
        std::size_t entries = 0;
        std::size_t persistent_hits = 0;
    };
    CacheStats cache_stats() const;


    /// Changes the per-call deadline, for a call already waiting as well as
    /// for later ones: it takes effect within one poll. Does not affect
    /// Config::startup_timeout.
    void set_timeout(std::chrono::milliseconds timeout);

    /// True while answers are read from and written to Config::cache_directory:
    /// a directory was configured, and no statement has changed Maxima's state
    /// since the process started.
    bool persistence_active() const;

    /// Replaces Maxima with a fresh process: every statement's change is
    /// discarded, the contexts made for assumptions are made again when next
    /// needed, and persistence resumes. Throws KernelError for a session built
    /// without a factory, which has no way to make another process.
    void restart();

    /// How many sets of assumptions keep a Maxima context at once.
    static constexpr std::size_t kMaxContexts = 16;

    /// How a failure to establish contradictory assumptions begins, so that
    /// to_result can give it Cause::Inconsistent.
    static constexpr std::string_view kInconsistent = "the assumptions are inconsistent: ";

    /// Builds the argv used to launch Maxima's SBCL image for `install`, with
    /// its paths in UTF-8. Exposed for testing; touches no filesystem and
    /// starts nothing.
    static std::vector<std::string> launch_command(const MaximaInstall &install);

    /// Builds the environment overrides layered over the parent's environment,
    /// with its paths in UTF-8 and forward slashes. Exposed for testing; may
    /// create Config::user_dir, or the default user directory, but starts
    /// nothing. Throws KernelError if the default directory exists and is not
    /// private to this user (see default_user_dir).
    static std::vector<EnvOverride> launch_environment(const MaximaInstall &install,
                                                      const Config &config);

    /// The statements sent once at startup to make the session machine-readable
    /// and deterministic. Exposed so tests can script a transport that expects
    /// exactly these.
    static std::vector<std::string> setup_statements();

    /// Wraps `payload` in the framed, error-trapping call sent to Maxima, for
    /// request `id` of the session whose frame key is `key`. Exposed for
    /// testing.
    static std::string request_for(std::string_view key, std::uint64_t id,
                                  const Payload &payload);

    /// Frame delimiters for request `id` under frame key `key`. Exposed so
    /// tests can script replies in the same shape Maxima produces.
    static std::string frame_begin(std::string_view key, std::uint64_t id);
    static std::string frame_separator(std::string_view key, std::uint64_t id);
    static std::string frame_end(std::string_view key, std::uint64_t id);

    /// The most a single reply may occupy before it is abandoned as a broken
    /// conversation. Without a limit, a child that streamed without ever
    /// closing its frame was bounded only by Config::timeout — at pipe speed,
    /// gigabytes. The largest reply measured in practice is under a megabyte.
    static constexpr std::size_t kMaxFrameBytes = std::size_t{256} * 1024 * 1024;

private:
    /// Which deadline a conversation runs on. A call's can be changed while it
    /// waits; the startup one, for the handshake and a replay, is fixed.
    enum class Deadline { Call, Startup };

    /// Sends one request and reads its frame. The pipe lock must be held.
    Reply eval_locked(const Payload &payload, Deadline deadline);

    /// Makes `environment`'s context current, establishing it first if it has
    /// none. A failed Reply if that could not be done, nothing if it was. The
    /// pipe lock must be held.
    std::optional<Reply> select_environment(const Environment &environment);

    /// Makes `name` Maxima's current context, if it is not already. The pipe
    /// lock must be held.
    Reply switch_context(const std::string &name);

    /// eval_locked on the call deadline, restarting the session if the
    /// conversation breaks down. The pipe lock must be held.
    Reply converse(const Payload &payload);

    /// The timeout `deadline` currently stands for.
    std::chrono::milliseconds timeout_for(Deadline deadline) const;

    /// Discards the dead transport, builds another, and handshakes. The
    /// contexts made for assumptions died with the old process.
    void recover();

    void handshake();
    void write_line(std::string_view line);

    /// Everything a persistent key has to be qualified by beyond the question
    /// and its assumptions: the two versions.
    std::string persistence_stamp() const;

    /// True when a persistent entry would describe this session honestly.
    bool using_persistence() const;

    /// Reads until the frame belonging to `id` is complete, discarding
    /// everything before it: banners, prompts, and any stale frame left over
    /// from an earlier request.
    Reply read_frame(std::uint64_t id, Deadline deadline);

    /// Two locks, always taken in this order when both are needed.
    ///
    /// The pipe lock is held for a whole conversation with Maxima, and guards
    /// the transport, the request ids and recovery. The state lock is only
    /// ever held briefly, and guards everything else that changes: the caches,
    /// the persistence flags and the per-call timeout. There used
    /// to be one lock for both, so reading a statistic or changing the timeout
    /// waited behind a computation for as long as Config::timeout.
    std::mutex pipe_mutex_;
    mutable std::mutex state_mutex_;

    /// Bumped by every invalidation. An answer is only cached if it was
    /// unchanged throughout the computation: invalidate_cache does not wait
    /// for the pipe, so it can happen mid-computation.
    std::uint64_t state_generation_ = 0;

    Config config_;
    TransportFactory factory_;
    std::unique_ptr<ITransport> transport_;
    std::uint64_t next_request_id_ = 0;
    std::string frame_key_ = random_frame_key();

    ReplyCache cache_;

    /// Set up once the Maxima version is known and Config::cache_directory is
    /// set. Absent means answers are remembered only for this process.
    std::unique_ptr<PersistentCache> persistent_;
    std::string maxima_version_;
    std::size_t persistent_hits_ = 0;

    /// False once a statement has changed Maxima's state in a way no key can
    /// describe, until the next restart.
    bool state_accounted_ = true;

    /// Guarded by the pipe lock: which context Maxima has current, empty when
    /// unknown (after a statement, which may have changed it), and the
    /// contexts made for assumptions, most recently used first.
    std::string active_context_ = "initial";
    std::list<std::pair<std::string, std::string>> contexts_; ///< (key, name)
    std::unordered_map<std::string, std::list<std::pair<std::string, std::string>>::iterator>
        context_index_;
    std::uint64_t next_context_ = 0;

    /// Set while recovering, so that a failure during the handshake or replay
    /// does not set off another recovery inside the first.
    bool recovering_ = false;
};

} // namespace proxima::detail
