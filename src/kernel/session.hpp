#pragma once

// Internal header. Note what is *not* here: this class holds no handles and
// includes no platform headers. It speaks the Maxima protocol over an
// ITransport and nothing else — the protocol itself is kernel/protocol.hpp,
// starting Maxima is kernel/launch.hpp, and which context a set of
// assumptions has is kernel/context_table.hpp. What is left is the
// conversation: sending requests, reading frames, keeping the caches honest,
// and putting the session back on its feet when Maxima dies.

#include "kernel/cache.hpp"
#include "kernel/context_table.hpp"
#include "kernel/persistent_cache.hpp"
#include "kernel/protocol.hpp"
#include "kernel/reply.hpp"
#include "transport/itransport.hpp"

#include <proxima/config.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace proxima::detail {

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

/// Drives a persistent Maxima session.
///
/// Maxima is reached directly through its underlying SBCL Lisp image, bypassing
/// maxima.bat//usr/bin/maxima entirely. That avoids spawning a fresh Maxima
/// process (and, on Windows, a flashing console window) per query. The shape
/// of what travels each way is documented in kernel/protocol.hpp.
class MaximaSession {
public:
    /// Produces a transport, and can be asked again after one dies. Holding a
    /// factory rather than a transport is what makes restarting possible.
    using TransportFactory = std::function<std::unique_ptr<ITransport>()>;

    /// Discovers Maxima as `config` says and launches it.
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
    static constexpr std::string_view kInconsistent
        = "the assumptions are inconsistent: ";

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

    /// Guarded by the pipe lock: which context Maxima has current — none when
    /// unknown, after a statement that may have changed it — and the contexts
    /// made for assumptions.
    std::optional<std::string> active_context_ = "initial";
    ContextTable contexts_{kMaxContexts};

    /// Set while recovering, so that a failure during the handshake or replay
    /// does not set off another recovery inside the first.
    bool recovering_ = false;
};

} // namespace proxima::detail
