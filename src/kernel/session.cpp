#include "kernel/session.hpp"

#include "kernel/discovery.hpp"
#include "transport/child_process.hpp"

#include <mx/errors.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <system_error>

namespace mx::detail {
namespace {

// How long to wait for any single chunk of output before checking whether the
// child is still alive. Not a deadline; see readFrame.
constexpr std::chrono::milliseconds kPollInterval{50};

// Maxima expects Windows paths with forward slashes; upstream's maxima.bat
// performs the same substitution before exporting maxima_prefix.
std::string toMaximaPath(const std::filesystem::path &p) {
    std::string text = p.string();
    std::replace(text.begin(), text.end(), '\\', '/');
    return text;
}

// The Lisp helpers installed at startup: one that does the framing, and one
// that stops Maxima asking questions.
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
// told exactly which assumption to supply — see mx::Context.
//
// `errcatch` hands `x` back as a Maxima list: empty on failure, one element on
// success. On failure the message is rendered by calling errormsg() with
// *standard-output* bound to a string, which is what keeps it inside the frame
// instead of loose in the stream. The request id appears in all three
// delimiters, so a frame can only ever be matched to the request that asked
// for it.
//
// Keep the delimiters here in step with frameBegin/frameSeparator/frameEnd
// below; a test asserts that they agree.
constexpr const char *kHelperLisp = R"LISP((progn
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
        (*print-readably* nil))
   (unless ok
    (let ((sink (make-string-output-stream)))
     (let ((*standard-output* sink)) (ignore-errors (maxima::$errormsg)))
     (setf reason (string-trim (list #\Space #\Newline #\Tab)
                               (get-output-stream-string sink)))))
   (format t "~&@@B~a@@~a@@S~a@@~s@@S~a@@~a@@E~a@@~%"
           id (if ok "T" "NIL") id (if ok (cadr x) nil) id reason id))
  (quote maxima::$done))
 (cl-user::run)))LISP";

std::unique_ptr<ITransport> launchMaxima(const Config &config) {
    const MaximaInstall install = discoverMaxima(config, systemEnv());
    return std::make_unique<ChildProcessTransport>(
        MaximaSession::launchCommand(install),
        MaximaSession::launchEnvironment(install, config));
}

} // namespace

std::string MaximaSession::frameBegin(std::uint64_t id) {
    return "@@B" + std::to_string(id) + "@@";
}

std::string MaximaSession::frameSeparator(std::uint64_t id) {
    return "@@S" + std::to_string(id) + "@@";
}

std::string MaximaSession::frameEnd(std::uint64_t id) {
    return "@@E" + std::to_string(id) + "@@";
}

std::vector<std::string> MaximaSession::setupStatements() {
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

std::string MaximaSession::requestFor(std::uint64_t id,
                                      std::string_view expression) {
    // errcatch turns a Maxima error into an empty list rather than an error
    // prompt; ratdisrep keeps canonical rational (MRAT) forms from coming back
    // in place of general ones.
    return "cppsend(" + std::to_string(id) + ", errcatch(ratdisrep("
           + std::string(expression) + ")))$";
}

std::vector<std::string>
MaximaSession::launchCommand(const MaximaInstall &install) {
    std::vector<std::string> argv{install.sbclExe.string(), "--core",
                                  install.maximaCore.string(), "--noinform"};

    if (install.raiseDynamicSpaceSize) {
        // What maxima.bat does on 64-bit builds, and for the same reason:
        // without the larger heap, load("lapack") runs out of dynamic space.
        argv.emplace_back("--dynamic-space-size");
        argv.emplace_back("2000");
    }

    argv.insert(argv.end(), {"--end-runtime-options", "--eval", kHelperLisp,
                             "--end-toplevel-options"});
    return argv;
}

std::vector<EnvOverride>
MaximaSession::launchEnvironment(const MaximaInstall &install,
                                 const Config &config) {
    std::vector<EnvOverride> env;

    // Correct even where the image already has a prefix compiled in, which
    // matters for a relocated or portable installation whose baked-in path no
    // longer exists.
    env.emplace_back("MAXIMA_PREFIX", toMaximaPath(install.root));

#ifdef _WIN32
    // Windows only, and deliberately so. The Windows bundle keeps sbcl.core
    // beside sbcl.exe and maxima.bat sets SBCL_HOME to that directory because
    // the crosscompiled installer does not. A distribution SBCL has its home
    // compiled in (/usr/lib/sbcl on openSUSE), which is *not* <root>/bin —
    // overriding it there would break contrib loading rather than fix it.
    env.emplace_back("SBCL_HOME", toMaximaPath(install.root / "bin"));
#endif

    if (!config.loadUserInit) {
        // Point Maxima's user directory somewhere we control so it does not
        // read the user's maxima-init.mac. See Config::loadUserInit.
        std::filesystem::path userDir = config.userDir;
        if (userDir.empty()) {
            std::error_code ec;
            userDir = std::filesystem::temp_directory_path(ec) / "maxima_cpp"
                      / "userdir";
        }
        std::error_code ec;
        std::filesystem::create_directories(userDir, ec);
        env.emplace_back("MAXIMA_USERDIR", toMaximaPath(userDir));
    }

    return env;
}

MaximaSession::MaximaSession(Config config)
    : config_(std::move(config)), transport_(launchMaxima(config_)) {
    handshake();
}

MaximaSession::MaximaSession(std::unique_ptr<ITransport> transport, Config config)
    : config_(std::move(config)), transport_(std::move(transport)) {
    if (!transport_) {
        throw KernelError("MaximaSession was given a null transport");
    }
    handshake();
}

MaximaSession::~MaximaSession() {
    if (transport_ && transport_->alive()) {
        // Ask Maxima to leave on its own; the transport terminates it if it
        // does not. Errors here are irrelevant, we are tearing down regardless.
        writeLine("quit();");
    }
    if (transport_) {
        transport_->kill();
    }
}

void MaximaSession::handshake() {
    for (const std::string &statement : setupStatements()) {
        writeLine(statement);
    }

    // A framed probe. Reading until *its* frame arrives is what synchronises
    // the stream: the startup banner and every prompt printed so far fall
    // before it and are discarded. No prompt markers are needed for this — the
    // frame delimiters already say exactly where a reply begins.
    const Reply ready = eval("true");
    if (!ready.ok) {
        throw KernelError("Maxima rejected the startup handshake: "
                          + ready.reason);
    }
}

Reply MaximaSession::eval(std::string_view expression) {
    const std::uint64_t id = ++nextRequestId_;
    writeLine(requestFor(id, expression));
    return readFrame(id);
}

void MaximaSession::writeLine(std::string_view line) {
    transport_->send(std::string(line) + "\n");
}

Reply MaximaSession::readFrame(std::uint64_t id) {
    const std::string begin = frameBegin(id);
    const std::string separator = frameSeparator(id);
    const std::string end = frameEnd(id);

    const auto deadline = std::chrono::steady_clock::now() + config_.timeout;

    std::string buffer;
    size_t endAt = std::string::npos;
    while ((endAt = buffer.find(end)) == std::string::npos) {
        const std::string chunk = transport_->receive(kPollInterval);
        if (!chunk.empty()) {
            buffer += chunk;
            continue;
        }
        // Nothing arrived. Either the child is gone, or it is simply still
        // thinking and we have time left to wait.
        if (!transport_->alive()) {
            throw KernelError("Maxima session ended unexpectedly");
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw KernelError("Maxima did not respond within the configured "
                              "timeout");
        }
    }

    const size_t beginAt = buffer.rfind(begin, endAt);
    if (beginAt == std::string::npos) {
        throw KernelError("Maxima produced a malformed reply: the closing "
                          "delimiter for request "
                          + std::to_string(id) + " arrived without its opening "
                          + "delimiter");
    }

    // Everything before `beginAt` is banner text, prompts, or a frame belonging
    // to some earlier request; none of it is our answer.
    const size_t bodyAt = beginAt + begin.size();
    const std::string body = buffer.substr(bodyAt, endAt - bodyAt);

    const size_t firstSeparator = body.find(separator);
    if (firstSeparator == std::string::npos) {
        throw KernelError("Maxima produced a malformed reply for request "
                          + std::to_string(id) + ": missing field separator");
    }
    const size_t secondSeparator
        = body.find(separator, firstSeparator + separator.size());
    if (secondSeparator == std::string::npos) {
        throw KernelError("Maxima produced a malformed reply for request "
                          + std::to_string(id) + ": missing second field "
                          + "separator");
    }

    Reply reply;
    reply.ok = body.compare(0, firstSeparator, "T") == 0;

    const size_t valueAt = firstSeparator + separator.size();
    const size_t reasonAt = secondSeparator + separator.size();

    if (reply.ok) {
        reply.value = body.substr(valueAt, secondSeparator - valueAt);
    } else {
        reply.reason = body.substr(reasonAt);
    }
    return reply;
}

} // namespace mx::detail
