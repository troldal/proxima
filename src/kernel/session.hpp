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
#include <proxima/reply.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
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
    /// helper and evaluated. What toMaxima produces.
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
std::string randomFrameKey();

/// Creates `dir` accessible to its owner only, or accepts it if it already
/// exists as a directory — not a link — owned by this user and closed to
/// everyone else; otherwise throws KernelError. On Windows, only creates it.
void ensurePrivateDirectory(const std::filesystem::path &dir);

/// The user directory Maxima is given when Config::userDir is empty: a
/// per-user private directory under the system temporary directory, checked
/// with ensurePrivateDirectory, since Maxima executes the maxima-init.mac it
/// finds there.
std::filesystem::path defaultUserDir();

class MaximaSession {
public:
    /// Produces a transport, and can be asked again after one dies. Holding a
    /// factory rather than a transport is what makes restarting possible.
    using TransportFactory = std::function<std::unique_ptr<ITransport>()>;

    /// Discovers Maxima under `config.maximaRoot` and launches it.
    explicit MaximaSession(Config config);

    /// Drives transports from `factory`, which is called again on restart.
    /// For tests, which pass a fixed `frameKey` so that scripted replies can
    /// be written in advance.
    MaximaSession(TransportFactory factory, Config config,
                  std::string frameKey = randomFrameKey());

    /// Drives one already-constructed transport, with no way to make another,
    /// so a death is final. For tests; `frameKey` as above.
    MaximaSession(std::unique_ptr<ITransport> transport, Config config,
                  std::string frameKey = randomFrameKey());

    ~MaximaSession();

    MaximaSession(const MaximaSession &) = delete;
    MaximaSession &operator=(const MaximaSession &) = delete;

    /// Evaluates one request and returns its result. A Maxima error —
    /// including a malformed expression, which fails inside the payload's
    /// own reader — is reported through Reply::ok rather than thrown.
    ///
    /// Throws TimeoutError if Maxima did not answer within Config::timeout, or
    /// KernelError if the session died or answered something unintelligible. In
    /// either case the kernel is restarted and its remembered state replayed
    /// first, so only this call is lost.
    ///
    /// Serialised: concurrent callers take turns rather than interleaving
    /// requests on one pipe. Calls that only touch bookkeeping — cacheStats,
    /// setTimeout, remember, forget, invalidateCache, persistenceActive — do
    /// not take that turn, and never wait behind a computation.
    Reply eval(const Payload &payload);

    /// Evaluates a pure expression, consulting and filling the reply cache.
    /// The caller guarantees the expression changes nothing in Maxima.
    Reply evalPure(const Payload &payload);

    /// Evaluates a state-changing statement the journal accounts for.
    Reply evalTracked(const Payload &payload);

    /// The requests a caller has to keep together, available only inside
    /// converseAtomically: a change to Maxima's state and the journal's record
    /// of it.
    class Conversation {
    public:
        Reply evalTracked(const Payload &payload);
        std::uint64_t remember(Payload payload);
        void forget(std::uint64_t handle);

    private:
        friend class MaximaSession;
        explicit Conversation(MaximaSession &session) : session_(session) {}
        MaximaSession &session_;
    };

    /// Runs `steps` as one conversation: no other caller's request reaches
    /// Maxima until it returns.
    ///
    /// What a statement and its record need. Made separately — evalTracked,
    /// then remember — another thread's evalPure can run between the two,
    /// compute under Maxima's new state, and file its answer under the
    /// journal's old one, where a persistent cache keeps it beyond this
    /// process. proxima::Context makes every change this way.
    void converseAtomically(const std::function<void(Conversation &)> &steps);

    /// Discards every cached reply.
    void invalidateCache();

    struct CacheStats {
        std::size_t hits = 0;
        std::size_t misses = 0;
        std::size_t entries = 0;
        std::size_t persistentHits = 0;
    };
    CacheStats cacheStats() const;

    /// Records a statement to replay after a restart, and returns a handle for
    /// removing it again.
    ///
    /// The kernel is a separate process holding mutable state — assumptions,
    /// declarations, bindings — that a restart would otherwise silently lose,
    /// leaving later results quietly wrong rather than obviously broken. Scoped
    /// state such as proxima::Context registers itself here.
    std::uint64_t remember(Payload payload);

    /// Stops replaying the statement `handle` names.
    void forget(std::uint64_t handle);

    /// Changes the per-call deadline, for a call already waiting as well as
    /// for later ones: it takes effect within one poll. Does not affect
    /// Config::startupTimeout.
    void setTimeout(std::chrono::milliseconds timeout);

    /// True while answers are read from and written to Config::cacheDirectory:
    /// a directory was configured, and nothing has changed Maxima's state
    /// without the journal recording it.
    bool persistenceActive() const;

    /// Replaces Maxima with a fresh process and replays the journal, which
    /// returns the session to exactly the state the journal describes: every
    /// unrecorded change, such as a raw eval's, is discarded, and persistence
    /// resumes. Throws KernelError for a session built without a factory, which
    /// has no way to make another process.
    void restart();

    /// Builds the argv used to launch Maxima's SBCL image for `install`, with
    /// its paths in UTF-8. Exposed for testing; touches no filesystem and
    /// starts nothing.
    static std::vector<std::string> launchCommand(const MaximaInstall &install);

    /// Builds the environment overrides layered over the parent's environment,
    /// with its paths in UTF-8 and forward slashes. Exposed for testing; may
    /// create Config::userDir, or the default user directory, but starts
    /// nothing. Throws KernelError if the default directory exists and is not
    /// private to this user (see defaultUserDir).
    static std::vector<EnvOverride> launchEnvironment(const MaximaInstall &install,
                                                      const Config &config);

    /// The statements sent once at startup to make the session machine-readable
    /// and deterministic. Exposed so tests can script a transport that expects
    /// exactly these.
    static std::vector<std::string> setupStatements();

    /// Wraps `payload` in the framed, error-trapping call sent to Maxima, for
    /// request `id` of the session whose frame key is `key`. Exposed for
    /// testing.
    static std::string requestFor(std::string_view key, std::uint64_t id,
                                  const Payload &payload);

    /// Frame delimiters for request `id` under frame key `key`. Exposed so
    /// tests can script replies in the same shape Maxima produces.
    static std::string frameBegin(std::string_view key, std::uint64_t id);
    static std::string frameSeparator(std::string_view key, std::uint64_t id);
    static std::string frameEnd(std::string_view key, std::uint64_t id);

    /// The most a single reply may occupy before it is abandoned as a broken
    /// conversation. Without a limit, a child that streamed without ever
    /// closing its frame was bounded only by Config::timeout — at pipe speed,
    /// gigabytes. The largest reply measured in practice is under a megabyte.
    static constexpr std::size_t kMaxFrameBytes = std::size_t{256} * 1024 * 1024;

private:
    struct JournalEntry {
        std::uint64_t handle;
        Payload payload;
    };

    /// Which deadline a conversation runs on. A call's can be changed while it
    /// waits; the startup one, for the handshake and a replay, is fixed.
    enum class Deadline { Call, Startup };

    /// Sends one request and reads its frame. The pipe lock must be held.
    Reply evalLocked(const Payload &payload, Deadline deadline);

    /// evalTracked's body. The pipe lock must be held.
    Reply evalTrackedLocked(const Payload &payload);

    /// evalLocked on the call deadline, restarting the session if the
    /// conversation breaks down. The pipe lock must be held.
    Reply converse(const Payload &payload);

    /// The timeout `deadline` currently stands for.
    std::chrono::milliseconds timeoutFor(Deadline deadline) const;

    /// Discards the dead transport, builds another, and restores the session:
    /// handshake, then every remembered statement in the order it was made.
    void recover();

    void handshake();
    void writeLine(std::string_view line);

    /// Everything a persistent key has to be qualified by: the two versions
    /// and the assumption state, in a form that changes whenever any of them
    /// does.
    std::string persistenceStamp() const;

    /// True when a persistent entry would describe this session honestly.
    bool usingPersistence() const;

    /// Rebuilds the persistent cache's stamp after the journal changed, so
    /// later entries are keyed on the new assumption state rather than the old.
    void restampPersistence();

    /// Reads until the frame belonging to `id` is complete, discarding
    /// everything before it: banners, prompts, and any stale frame left over
    /// from an earlier request.
    Reply readFrame(std::uint64_t id, Deadline deadline);

    /// Two locks, always taken in this order when both are needed.
    ///
    /// The pipe lock is held for a whole conversation with Maxima, and guards
    /// the transport, the request ids and recovery. The state lock is only
    /// ever held briefly, and guards everything else that changes: the caches,
    /// the journal, the persistence flags and the per-call timeout. There used
    /// to be one lock for both, so reading a statistic or changing the timeout
    /// waited behind a computation for as long as Config::timeout.
    std::mutex pipeMutex_;
    mutable std::mutex stateMutex_;

    /// Bumped by every change to Maxima's state or to the journal. An answer
    /// is only cached if it was unchanged throughout the computation: with the
    /// journal no longer waiting for the pipe, it can change mid-computation.
    std::uint64_t stateGeneration_ = 0;

    Config config_;
    TransportFactory factory_;
    std::unique_ptr<ITransport> transport_;
    std::uint64_t nextRequestId_ = 0;
    std::string frameKey_ = randomFrameKey();

    ReplyCache cache_;

    /// Set up once the Maxima version is known and Config::cacheDirectory is
    /// set. Absent means answers are remembered only for this process.
    std::unique_ptr<PersistentCache> persistent_;
    std::string maximaVersion_;
    std::size_t persistentHits_ = 0;

    /// False once something has changed Maxima's state without the journal
    /// recording it, which makes a persistent key unable to describe the
    /// session it was computed in.
    bool stateAccounted_ = true;

    std::vector<JournalEntry> journal_;
    std::uint64_t nextJournalHandle_ = 0;

    /// Set while recovering, so that a failure during the handshake or replay
    /// does not set off another recovery inside the first.
    bool recovering_ = false;
};

} // namespace proxima::detail
