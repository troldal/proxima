#include "kernel/session.hpp"

#include "kernel/discovery.hpp"
#include "transport/child_process.hpp"
#include "util/utf8.hpp"
#include "wire/to_maxima.hpp"

#include <proxima/errors.hpp>
#include <proxima/version.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <exception>
#include <filesystem>
#include <format>
#include <random>
#include <string>
#include <system_error>

#ifndef _WIN32
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace proxima::detail {
namespace {

// How long to wait for any single chunk of output before checking whether the
// child is still alive. Not a deadline; see read_frame.
constexpr std::chrono::milliseconds kPollInterval{50};

// Maxima expects Windows paths with forward slashes; upstream's maxima.bat
// performs the same substitution before exporting maxima_prefix. UTF-8, like
// every string handed to the transport; '\\' is ASCII, so replacing it in the
// encoded bytes cannot touch part of a longer character.
std::string to_maxima_path(const std::filesystem::path &p) {
    std::string text = to_utf8(p);
    std::replace(text.begin(), text.end(), '\\', '/');
    return text;
}

// The Lisp helpers installed at startup: one that reads a form and evaluates
// it, one that does the framing, and one that stops Maxima asking questions.
//
// `$cppread` is the outbound half of the s-expression protocol. It reads one
// Lisp form from the string it is given — with *read-eval* off, so `#.` cannot
// run code, and with the Maxima package current, so `MPLUS` and `$X` land on
// the symbols Maxima uses — and hands it to meval. Because it is called from
// inside errcatch, a form that fails to read or to evaluate is an ordinary
// Maxima error with a message, not a silence.
//
// One thing Maxima's parser does that reading a form does not: resolve
// aliases. `subst` is an alias for `substitute`, and a form headed `$SUBST`
// would evaluate to itself, unrecognised, where the text `subst(...)` would
// have been rewritten on the way in. `cppresolve` walks the form applying
// Maxima's own `getalias` to every symbol, which is exactly what the parser
// does — including `$true` and `$false` to their Lisp spellings.
//
// Maxima interrogates the user when it needs a fact it has not been told —
// `integrate(x^n, x)` asks "Is n equal to -1?" — by printing a prompt and
// reading a line from standard input. Over a pipe that is fatal twice over: the
// read blocks until the timeout, and then Maxima consumes the *next request* as
// the answer, leaving every subsequent reply attached to the wrong question.
//
// Overriding `retrieve`, the single point all of that goes through, turns a
// question into an ordinary Maxima error. errcatch then reports it as a Failure
// carrying the question text, the session stays synchronised, and the caller is
// told exactly which assumption to supply — see proxima::Context.
//
// `errcatch` hands `x` back as a Maxima list: empty on failure, one element on
// success. On failure the message is rendered by calling errormsg() with
// *standard-output* bound to a string, which is what keeps it inside the frame
// instead of loose in the stream. The request id appears in all three
// delimiters, so a frame can only ever be matched to the request that asked
// for it.
//
// The reply is printed with Lisp's print limits switched off. Maxima itself
// runs with *print-length* at 100 and *print-level* at 15, and under those a
// sum of more than a hundred terms printed as its first hundred and `...`, and
// anything nested deeper than fifteen levels as `#`. The reader took both for
// symbols, so a large result came back as a smaller, wrong one that looked
// right. *print-base* and *print-radix* are pinned for the same reason: what is
// printed here is data for a reader, not text for a person.
//
// Keep the delimiters here in step with frame_begin/frame_separator/frame_end
// below; a test asserts that they agree.
constexpr const char *kHelperLisp = R"LISP((progn
 (defun maxima::cppresolve (form)
  (cond ((symbolp form) (maxima::getalias form))
        ((atom form) form)
        (t (mapcar (function maxima::cppresolve) form))))
 (defun maxima::$cppread (text)
  (let ((*package* (find-package :maxima))
        (*read-eval* nil)
        (*read-base* 10)
        (*read-default-float-format* 'double-float)
        (*readtable* (copy-readtable nil)))
   (maxima::meval (maxima::cppresolve (read-from-string text)))))
 (defun maxima::retrieve (msg flag &rest more)
  (declare (ignore flag more))
  (maxima::merror
   "this computation needs an assumption that was not supplied. Maxima asked: ~a"
   (with-output-to-string (s)
    (dolist (part (cond ((not (listp msg)) (list msg))
                        ((consp (car msg)) (cdr msg))
                        (t msg)))
     (princ (if (stringp part) part (maxima::$sconcat part)) s)))))
 (defun maxima::$cppsend (id x)
  (let ((ok (and (consp x) (cdr x)))
        (reason "")
        (*print-circle* nil)
        (*print-pretty* nil)
        (*print-readably* nil)
        (*print-length* nil)
        (*print-level* nil)
        (*print-lines* nil)
        (*print-base* 10)
        (*print-radix* nil))
   (unless ok
    (let ((sink (make-string-output-stream)))
     (let ((*standard-output* sink)) (ignore-errors (maxima::$errormsg)))
     (setf reason (string-trim (list #\Space #\Newline #\Tab)
                               (get-output-stream-string sink)))))
   (format t "~&@@B~a@@~a@@S~a@@~s@@S~a@@~a@@E~a@@~%"
           id (if ok "T" "NIL") id (if ok (cadr x) nil) id reason id))
  (quote maxima::$done))
 (cl-user::run)))LISP";

std::unique_ptr<ITransport> launch_maxima(const Config &config,
                                          std::string &version_tag) {
    const MaximaInstall install
        = discover_maxima(config, system_env(), system_command());
    version_tag = install.version_tag;

    // SBCL's runtime opens its executable and core by the names on its
    // command line, which it reads through the ANSI API on Windows; those two
    // get a spelling it can open. The root stays as it is: it only reaches the
    // environment, which SBCL and Maxima read in full Unicode.
    MaximaInstall launchable = install;
    launchable.sbcl_exe = sbcl_readable_path(install.sbcl_exe);
    launchable.maxima_core = sbcl_readable_path(install.maxima_core);

    return std::make_unique<ChildProcessTransport>(
        MaximaSession::launch_command(launchable),
        MaximaSession::launch_environment(launchable, config));
}

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

namespace {

// What the helper formats with ~a in every delimiter.
std::string frame_tag(std::string_view key, std::uint64_t id) {
    return std::format("{}-{}", key, id);
}

} // namespace

std::string random_frame_key() {
    std::random_device device;
    const std::uint64_t bits
        = (std::uint64_t{device()} << 32) ^ std::uint64_t{device()};
    return std::format("{:016x}", bits);
}

std::string MaximaSession::frame_begin(std::string_view key, std::uint64_t id) {
    return "@@B" + frame_tag(key, id) + "@@";
}

std::string MaximaSession::frame_separator(std::string_view key, std::uint64_t id) {
    return "@@S" + frame_tag(key, id) + "@@";
}

std::string MaximaSession::frame_end(std::string_view key, std::uint64_t id) {
    return "@@E" + frame_tag(key, id) + "@@";
}

std::vector<std::string> MaximaSession::setup_statements() {
    return {
        // Results as one-dimensional text rather than ASCII art. Irrelevant to
        // the framed values themselves, but it keeps anything Maxima prints
        // outside a frame from becoming a wall of layout.
        "display2d:false$",
        // Maxima otherwise retains every %i/%o label for the life of the
        // session, which for a long-lived kernel is an unbounded leak.
        "nolabels:true$",
        // Errors are rendered into the frame by the helper instead; without
        // this they would also be printed loose in the stream.
        "errormsg:false$",
    };
}

Payload Payload::form(std::string_view sexpr) {
    return Payload("cppread(" + string_literal(sexpr) + ")");
}

Payload Payload::text(std::string_view source) {
    return Payload("eval_string(" + string_literal(source) + ")");
}

std::string MaximaSession::request_for(std::string_view key, std::uint64_t id,
                                       const Payload &payload) {
    // errcatch turns a Maxima error into an empty list rather than an error
    // prompt; ratdisrep keeps canonical rational (MRAT) forms from coming back
    // in place of general ones. The payload is a call on a string literal, so
    // this text is well-formed whatever the caller asked. The tag travels as a
    // Maxima string, which is a Lisp string by the time the helper prints it
    // with ~a — without quotes, exactly as frame_begin spells it.
    return "cppsend(" + string_literal(frame_tag(key, id)) + ", errcatch(ratdisrep("
           + payload.str() + ")))$";
}

std::vector<std::string>
MaximaSession::launch_command(const MaximaInstall &install) {
    // UTF-8, as the transport takes every string. path::string() would be the
    // ANSI code page on Windows, which mangles a Maxima installed under a path
    // outside it before SBCL ever sees it.
    std::vector<std::string> argv{to_utf8(install.sbcl_exe), "--core",
                                  to_utf8(install.maxima_core), "--noinform"};

    if (install.raise_dynamic_space_size) {
        // What maxima.bat does on 64-bit builds, and for the same reason:
        // without the larger heap, load("lapack") runs out of dynamic space.
        argv.emplace_back("--dynamic-space-size");
        argv.emplace_back("2000");
    }

    // --disable-debugger is a *toplevel* option, not a runtime one, so it
    // belongs after --end-runtime-options. Without it, an unhandled Lisp error
    // drops SBCL into a debugger that reads standard input — over a pipe, a
    // deadlock. With it, the process exits instead, which recover() can undo.
    // errcatch is unaffected: it handles the error before the debugger would
    // ever see it.
    argv.insert(argv.end(), {"--end-runtime-options", "--disable-debugger", "--eval",
                             kHelperLisp, "--end-toplevel-options"});
    return argv;
}

std::vector<EnvOverride>
MaximaSession::launch_environment(const MaximaInstall &install,
                                  const Config &config) {
    std::vector<EnvOverride> env;

    // Correct even where the image already has a prefix compiled in, which
    // matters for a relocated or portable installation whose baked-in path no
    // longer exists.
    env.emplace_back("MAXIMA_PREFIX", to_maxima_path(install.root));

#ifdef _WIN32
    // Windows only, and deliberately so. The Windows bundle keeps sbcl.core
    // beside sbcl.exe and maxima.bat sets SBCL_HOME to that directory because
    // the crosscompiled installer does not. A distribution SBCL has its home
    // compiled in (/usr/lib/sbcl on openSUSE), which is *not* <root>/bin —
    // overriding it there would break contrib loading rather than fix it.
    env.emplace_back("SBCL_HOME", to_maxima_path(install.sbcl_exe.parent_path()));
#endif

    if (!config.load_user_init) {
        // Point Maxima's user directory somewhere we control so it does not
        // read the user's maxima-init.mac. See Config::load_user_init.
        std::filesystem::path user_dir = config.user_dir;
        if (user_dir.empty()) {
            user_dir = default_user_dir();
        } else {
            std::error_code ec;
            std::filesystem::create_directories(user_dir, ec);
        }
        env.emplace_back("MAXIMA_USERDIR", to_maxima_path(user_dir));
    }

    return env;
}

void ensure_private_directory(const std::filesystem::path &dir) {
#ifdef _WIN32
    // %TEMP% is under the user's own profile, which other users cannot write.
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
#else
    const auto refuse = [&dir](const std::string &why) {
        throw KernelError("refusing to use " + to_utf8(dir)
                          + " as Maxima's user directory: " + why);
    };

    // mkdir with the mode, rather than create_directories and a chmod after,
    // so there is no moment at which the directory exists and is open to
    // others. An existing one is accepted only if it is already private.
    if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
        refuse(std::generic_category().message(errno));
    }

    // lstat, not stat: a symbolic link planted in its place is refused rather
    // than followed to wherever its owner chose.
    struct stat info{};
    if (::lstat(dir.c_str(), &info) != 0) {
        refuse(std::generic_category().message(errno));
    }
    if (!S_ISDIR(info.st_mode)) {
        refuse("it is not a directory");
    }
    if (info.st_uid != ::geteuid()) {
        refuse("it belongs to another user");
    }
    if ((info.st_mode & static_cast<mode_t>(0077)) != 0) {
        refuse("other users have access to it");
    }
#endif
}

std::filesystem::path default_user_dir() {
    std::error_code ec;
    const std::filesystem::path temp = std::filesystem::temp_directory_path(ec);

#ifdef _WIN32
    const std::filesystem::path user_dir = temp / "proxima" / "userdir";
    ensure_private_directory(user_dir);
#else
    // Maxima runs whatever maxima-init.mac it finds here. /tmp is shared by
    // every user of the machine, so a single /tmp/proxima/userdir — what this
    // used to be — let whoever created it first run code in every other
    // user's Proxima. One directory per user, and only if it is really theirs.
    const std::filesystem::path base
        = temp / ("proxima-" + std::to_string(::geteuid()));
    ensure_private_directory(base);
    const std::filesystem::path user_dir = base / "userdir";
    ensure_private_directory(user_dir);
#endif
    return user_dir;
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
        // Not `version`, which would hide proxima::version (MSVC's C4459).
        std::string launched_version;
        auto transport = launch_maxima(config_, launched_version);
        const std::lock_guard<std::mutex> state(state_mutex_);
        maxima_version_ = std::move(launched_version);
        return transport;
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
    active_context_ = reply.ok ? name : std::string();
    return reply;
}

std::optional<Reply>
MaximaSession::select_environment(const Environment &environment) {
    if (environment.key.empty()) {
        const Reply switched = switch_context("initial");
        return switched.ok ? std::nullopt : std::optional<Reply>(switched);
    }

    if (const auto found = context_index_.find(environment.key);
        found != context_index_.end()) {
        contexts_.splice(contexts_.begin(), contexts_, found->second);
        const Reply switched = switch_context(found->second->second);
        return switched.ok ? std::nullopt : std::optional<Reply>(switched);
    }

    // A context of its own, under `initial` so that what a statement put
    // there is in force here too. supcontext makes it current.
    const std::string name = "proxima_a" + std::to_string(++next_context_);
    Reply made = converse(Payload::text("supcontext(" + name + ", initial)"));
    if (!made.ok) {
        active_context_.clear();
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

    contexts_.emplace_front(environment.key, name);
    context_index_.emplace(environment.key, contexts_.begin());
    while (contexts_.size() > kMaxContexts) {
        // Never the one just made, which is at the front and current.
        const auto &[key, victim] = contexts_.back();
        const Reply killed = converse(Payload::text("killcontext(" + victim + ")"));
        static_cast<void>(
            killed); // A context Maxima no longer has is gone either way.
        context_index_.erase(key);
        contexts_.pop_back();
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
    context_index_.clear();

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
    const std::string begin = frame_begin(frame_key_, id);
    const std::string separator = frame_separator(frame_key_, id);
    const std::string end = frame_end(frame_key_, id);

    const auto started = std::chrono::steady_clock::now();

    std::string buffer;
    // Where the next search for the closing delimiter starts. Only the bytes
    // that just arrived can complete it, plus the few before them where a
    // delimiter split across two reads would begin. Searching the whole buffer
    // after every read made a large reply cost time quadratic in its size.
    std::size_t search_from = 0;
    size_t end_at = std::string::npos;
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

    const size_t begin_at = buffer.rfind(begin, end_at);
    if (begin_at == std::string::npos) {
        throw KernelError("Maxima produced a malformed reply: the closing "
                          "delimiter for request "
                          + frame_tag(frame_key_, id)
                          + " arrived without its opening " + "delimiter");
    }

    // Everything before `begin_at` is banner text, prompts, or a frame belonging
    // to some earlier request; none of it is our answer.
    const size_t body_at = begin_at + begin.size();
    const std::string body = buffer.substr(body_at, end_at - body_at);

    const size_t first_separator = body.find(separator);
    if (first_separator == std::string::npos) {
        throw KernelError("Maxima produced a malformed reply for request "
                          + std::to_string(id) + ": missing field separator");
    }
    const size_t second_separator
        = body.find(separator, first_separator + separator.size());
    if (second_separator == std::string::npos) {
        throw KernelError("Maxima produced a malformed reply for request "
                          + std::to_string(id) + ": missing second field "
                          + "separator");
    }

    Reply reply;
    reply.ok = body.compare(0, first_separator, "T") == 0;

    const size_t value_at = first_separator + separator.size();
    const size_t reason_at = second_separator + separator.size();

    if (reply.ok) {
        reply.value = body.substr(value_at, second_separator - value_at);
    } else {
        reply.reason = body.substr(reason_at);
    }
    return reply;
}

} // namespace proxima::detail
