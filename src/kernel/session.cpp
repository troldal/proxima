#include "kernel/session.hpp"

#include "kernel/launch.hpp"
#include "kernel/protocol.hpp"

#include <proxima/errors.hpp>
#include <proxima/version.hpp>

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>

namespace proxima::detail {
namespace {

// How long to wait for any single chunk of output before checking whether the
// child is still alive. Not a deadline; see read_frame.
constexpr std::chrono::milliseconds kPollInterval{50};

} // namespace

std::string MaximaSession::persistence_stamp() const {
    // Everything an answer depends on beyond the question and the assumptions
    // it was asked under, which are both in each entry's key. The assumptions
    // used to be here too, as the journal of what was in force, which made
    // every key depend on whatever scopes happened to be open.
    std::string stamp = "lib=";
    stamp += version;
    stamp += "\nmaxima=" + maxima_version_;
    return stamp;
}

bool MaximaSession::using_persistence() const {
    return persistent_ != nullptr && persistent_->usable() && state_accounted_;
}

MaximaSession::MaximaSession(Config config)
    : config_(std::move(config)),
      cache_(config_.cache_entries, config_.cache_bytes) {
    // A factory rather than one transport, so a dead kernel can be replaced.
    //
    // The version is recorded under the state lock: a restart relaunches from
    // inside a conversation, holding only the pipe lock, while another thread
    // may be reading the version to form a persistent key.
    factory_ = [this] {
        Launched launched = launch_maxima(config_);
        const std::lock_guard<std::mutex> state(state_mutex_);
        maxima_version_ = std::move(launched.version_tag);
        return std::move(launched.transport);
    };
    transport_ = factory_();
    handshake();

    // Only now is the Maxima version known, and a persistent key cannot be
    // formed without it.
    const std::lock_guard<std::mutex> state(state_mutex_);
    if (!config_.cache_directory.empty() && !maxima_version_.empty()) {
        persistent_ = std::make_unique<PersistentCache>(
            config_.cache_directory, persistence_stamp(),
            config_.cache_directory_limit);
    }
}

MaximaSession::MaximaSession(TransportFactory factory, Config config,
                             std::string frame_key)
    : config_(std::move(config)), factory_(std::move(factory)),
      frame_key_(std::move(frame_key)),
      cache_(config_.cache_entries, config_.cache_bytes) {
    if (!factory_) {
        throw KernelError("MaximaSession was given a null transport factory");
    }
    transport_ = factory_();
    if (!transport_) {
        throw KernelError("the transport factory produced nothing");
    }
    handshake();
}

MaximaSession::MaximaSession(std::unique_ptr<ITransport> transport, Config config,
                             std::string frame_key)
    : config_(std::move(config)), transport_(std::move(transport)),
      frame_key_(std::move(frame_key)),
      cache_(config_.cache_entries, config_.cache_bytes) {
    // No factory, so this session cannot be restarted; a death is final.
    if (!transport_) {
        throw KernelError("MaximaSession was given a null transport");
    }
    handshake();
}

MaximaSession::~MaximaSession() {
    if (transport_ && transport_->alive()) {
        // Ask Maxima to leave on its own; the transport terminates it if it
        // does not. Errors here are irrelevant, we are tearing down regardless.
        write_line("quit();");
    }
    if (transport_) {
        transport_->kill();
    }
}

void MaximaSession::handshake() {
    for (const std::string &statement : setup_statements()) {
        write_line(statement);
    }

    // A framed probe. Reading until *its* frame arrives is what synchronises
    // the stream: the startup banner and every prompt printed so far fall
    // before it and are discarded. No prompt markers are needed for this — the
    // frame delimiters already say exactly where a reply begins.
    // A form rather than text, so the handshake depends on nothing but the
    // helper itself — eval_string lives in a package Maxima autoloads.
    const Reply ready = eval_locked(Payload::form("T"), Deadline::Startup);
    if (!ready.ok) {
        throw KernelError("Maxima rejected the startup handshake: " + ready.reason);
    }
}

Reply MaximaSession::converse(const Payload &payload) {
    try {
        return eval_locked(payload, Deadline::Call);
    } catch (const KernelError &) {
        // The conversation broke down. Put the session back on its feet before
        // reporting, so that only this call is lost rather than every call
        // after it. Recovery failing is not worth replacing the original
        // diagnosis with — the next call will try again.
        if (factory_ && !recovering_) {
            try {
                recover();
            } catch (...) {
            }
        }
        throw;
    }
}

Reply MaximaSession::eval(const Payload &payload) {
    const std::lock_guard<std::mutex> pipe(pipe_mutex_);
    {
        const std::lock_guard<std::mutex> state(state_mutex_);
        // This entry point can evaluate anything, including a statement that
        // changes Maxima's state, and nothing in the text says which. Assuming
        // the worst is the only safe default: a stale cached answer is a
        // correctness bug, an emptied cache is merely slower.
        cache_.clear();
        // And a change no key describes means a persistent entry could claim
        // conditions that do not hold. Persistence stops here, until restart.
        state_accounted_ = false;
        ++state_generation_;
    }
    // Outside every context made for assumptions, so that what a statement
    // assumes or declares is not filed in one of them and lost when it is
    // evicted.
    if (const Reply switched = switch_context("initial"); !switched.ok) {
        return switched;
    }
    // The statement runs in `initial`, and the session goes on assuming it is
    // still current: a statement that switches context itself is changing
    // what questions without assumptions are asked under, which is its
    // caller's business, and the caches it could mislead were emptied above.
    return converse(payload);
}

Reply MaximaSession::switch_context(const std::string &name) {
    if (active_context_ == name) {
        return {true, "", ""};
    }
    // Names are this session's own or `initial`, never the caller's, so
    // they can travel as text.
    Reply reply = converse(Payload::text("context: " + name));
    if (reply.ok) {
        active_context_ = name;
    } else {
        active_context_.reset(); // Whatever Maxima has current, it is not known.
    }
    return reply;
}

std::optional<Reply>
MaximaSession::select_environment(const Environment &environment) {
    if (environment.key.empty()) {
        const Reply switched = switch_context("initial");
        return switched.ok ? std::nullopt : std::optional<Reply>(switched);
    }

    if (const std::optional<std::string> existing
        = contexts_.find(environment.key)) {
        const Reply switched = switch_context(*existing);
        return switched.ok ? std::nullopt : std::optional<Reply>(switched);
    }

    // A context of its own, under `initial` so that what a statement put
    // there is in force here too. supcontext makes it current.
    const std::string name = contexts_.next_name();
    Reply made = converse(Payload::text("supcontext(" + name + ", initial)"));
    if (!made.ok) {
        active_context_.reset();
        return made;
    }
    active_context_ = name;

    // A statement that fails, or facts that contradict one another, leave no
    // context behind: a later call under the same assumptions tries again,
    // and gets the same answer.
    const auto discard = [&] {
        switch_context("initial");
        converse(Payload::text("killcontext(" + name + ")"));
    };
    for (const Environment::Statement &statement : environment.statements) {
        Reply reply = converse(statement.payload);
        if (!reply.ok) {
            discard();
            return reply;
        }
        // assume answers with a list saying what it did with each fact; a
        // fact that contradicts those already in force is `inconsistent`, and
        // is not added.
        if (reply.value == "((MLIST SIMP) $INCONSISTENT)") {
            discard();
            return Reply{false, "",
                         std::string(kInconsistent) + statement.description
                             + " contradicts the assumptions before it"};
        }
    }

    // Never the one just made, which is at the front and current.
    for (const std::string &victim : contexts_.insert(environment.key, name)) {
        const Reply killed = converse(Payload::text("killcontext(" + victim + ")"));
        static_cast<void>(
            killed); // A context Maxima no longer has is gone either way.
    }
    return std::nullopt;
}

Reply MaximaSession::eval_locked(const Payload &payload, Deadline deadline) {
    const std::uint64_t id = ++next_request_id_;
    write_line(request_for(frame_key_, id, payload));
    return read_frame(id, deadline);
}

Reply MaximaSession::eval_pure(const Payload &payload,
                               const Environment &environment) {
    // The pipe lock for the whole call, cache lookups included, so that no
    // statement changes Maxima between a question and the caching of its
    // answer.
    const std::lock_guard<std::mutex> pipe(pipe_mutex_);

    // The question and the assumptions it is asked under. With none, the
    // question alone, as it always was.
    const std::string key = environment.key.empty()
                                ? payload.str()
                                : environment.key + '\x1f' + payload.str();
    std::uint64_t generation = 0;
    {
        const std::lock_guard<std::mutex> state(state_mutex_);
        if (const Reply *cached = cache_.find(key)) {
            return *cached;
        }
        if (using_persistence()) {
            if (auto stored = persistent_->find(key)) {
                // Promoted into memory as well, so a second ask costs nothing.
                ++persistent_hits_;
                cache_.insert(key, *stored);
                return *stored;
            }
        }
        generation = state_generation_;
    }

    // Not cached: a failure to establish the assumptions is not an answer to
    // the question, and trying again is what should happen next time.
    if (std::optional<Reply> refused = select_environment(environment)) {
        return *refused;
    }
    const Reply reply = converse(payload);

    const std::lock_guard<std::mutex> state(state_mutex_);
    // Only kept if nothing was invalidated while Maxima was working:
    // invalidate_cache does not wait for the pipe.
    if (state_generation_ == generation) {
        // Failures are cached too: "Maxima cannot integrate this" is as stable
        // an answer as any other, and re-asking costs the same round trip.
        cache_.insert(key, reply);
        if (using_persistence()) {
            persistent_->insert(key, reply);
        }
    }
    return reply;
}

void MaximaSession::invalidate_cache() {
    const std::lock_guard<std::mutex> state(state_mutex_);
    cache_.clear();
    ++state_generation_;
}

MaximaSession::CacheStats MaximaSession::cache_stats() const {
    // The state lock only, so asking does not wait behind a computation. It
    // used to share one lock with every evaluation, and could block for the
    // whole of Config::timeout behind a slow integral.
    const std::lock_guard<std::mutex> state(state_mutex_);
    return {cache_.hits(), cache_.misses(), cache_.size(), persistent_hits_};
}

void MaximaSession::set_timeout(std::chrono::milliseconds timeout) {
    // Takes effect for a call already waiting, too: read_frame re-reads the
    // timeout on every poll. With one shared lock this used to wait for that
    // very call to finish, so it could never shorten it.
    const std::lock_guard<std::mutex> state(state_mutex_);
    config_.timeout = timeout;
}

void MaximaSession::recover() {
    recovering_ = true;
    struct Restore {
        bool &flag;
        ~Restore() { flag = false; }
    } restore{recovering_};

    // terminate, not kill: the process being replaced was never asked to quit.
    // After a timeout it is still busy computing and will not leave on its own,
    // so kill's grace period was two seconds added to every recovery for
    // nothing; after a death there is nothing left to wait for anyway.
    if (transport_) {
        transport_->terminate();
    }
    transport_ = factory_();
    if (!transport_) {
        throw KernelError("could not restart Maxima: the transport factory "
                          "produced nothing");
    }

    // A new process over a new pipe, so no reply from the old one can reach us
    // and ids can safely start over. Continuing to climb would work equally
    // well; starting fresh just makes a transcript easier to follow.
    next_request_id_ = 0;

    handshake();

    // The contexts made for assumptions died with the old process; they are
    // made again when next asked for. So did whatever a statement changed,
    // which is why persistence can resume: the new process is exactly what
    // the keys describe. It used to stay off for good, even across restarts
    // that had discarded the change. The in-memory answers belonged to the
    // old process and go with it.
    active_context_ = "initial";
    contexts_.clear();

    const std::lock_guard<std::mutex> state(state_mutex_);
    cache_.clear();
    ++state_generation_;
    state_accounted_ = true;
}

void MaximaSession::restart() {
    const std::lock_guard<std::mutex> pipe(pipe_mutex_);
    if (!factory_) {
        throw KernelError("this session cannot be restarted: it was built "
                          "without a way to start another Maxima");
    }
    recover();
}

bool MaximaSession::persistence_active() const {
    const std::lock_guard<std::mutex> state(state_mutex_);
    return using_persistence();
}

void MaximaSession::write_line(std::string_view line) {
    transport_->send(std::string(line) + "\n");
}

std::chrono::milliseconds MaximaSession::timeout_for(Deadline deadline) const {
    if (deadline == Deadline::Startup) {
        // Fixed at construction, so there is nothing to lock.
        return config_.startup_timeout;
    }
    const std::lock_guard<std::mutex> state(state_mutex_);
    return config_.timeout;
}

Reply MaximaSession::read_frame(std::uint64_t id, Deadline deadline_kind) {
    const std::string end = frame_end(frame_key_, id);
    const auto started = std::chrono::steady_clock::now();

    std::string buffer;
    // Where the next search for the closing delimiter starts. Only the bytes
    // that just arrived can complete it, plus the few before them where a
    // delimiter split across two reads would begin. Searching the whole buffer
    // after every read made a large reply cost time quadratic in its size.
    std::size_t search_from = 0;
    std::size_t end_at = std::string::npos;
    while ((end_at = buffer.find(end, search_from)) == std::string::npos) {
        // Recomputed every time round, so set_timeout can shorten a call that is
        // already waiting. And checked every time round, not only when a read
        // comes back empty: a reply that arrives as a slow but unbroken trickle
        // would otherwise never test the deadline at all.
        const auto deadline = started + timeout_for(deadline_kind);
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            throw TimeoutError("Maxima did not respond within the configured "
                               "timeout");
        }

        // Never wait past the deadline, so it is honoured to within a poll
        // rather than overshot by one.
        const auto remaining
            = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const std::string chunk
            = transport_->receive(std::min(kPollInterval, remaining));
        if (!chunk.empty()) {
            search_from
                = buffer.size() >= end.size() ? buffer.size() - (end.size() - 1) : 0;
            buffer += chunk;
            // A KernelError, so converse restarts the child: whatever it was
            // printing, the stream can no longer be trusted to line up.
            if (buffer.size() > kMaxFrameBytes) {
                throw KernelError(
                    "Maxima's reply exceeded "
                    + std::to_string(kMaxFrameBytes / (std::size_t{1024} * 1024))
                    + " MB without completing");
            }
            continue;
        }
        // Nothing arrived. Either the child is gone, or it is simply still
        // thinking and we have time left to wait.
        if (!transport_->alive()) {
            throw KernelError("Maxima session ended unexpectedly");
        }
    }

    return parse_frame(buffer, end_at, frame_key_, id);
}

} // namespace proxima::detail
