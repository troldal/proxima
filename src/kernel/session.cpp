#include "kernel/session.hpp"

#include "kernel/discovery.hpp"
#include "transport/child_process.hpp"

#include <mx/errors.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <regex>
#include <system_error>

namespace mx::detail {
namespace {

std::string trim(const std::string &s) {
    size_t first = s.find_first_not_of(" \t\r\n");
    size_t last = s.find_last_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    return s.substr(first, last - first + 1);
}

// How long to wait for any single chunk of output before checking whether the
// child is still alive. Not a deadline; see readUntilPrompt.
constexpr std::chrono::milliseconds kPollInterval{50};

// Maxima expects Windows paths with forward slashes; upstream's maxima.bat
// performs the same substitution before exporting maxima_prefix.
std::string toMaximaPath(const std::filesystem::path &p) {
    std::string text = p.string();
    std::replace(text.begin(), text.end(), '\\', '/');
    return text;
}

std::unique_ptr<ITransport> launchMaxima(const Config &config) {
    const MaximaInstall install = discoverMaxima(config, systemEnv());
    return std::make_unique<ChildProcessTransport>(
        MaximaSession::launchCommand(install),
        MaximaSession::launchEnvironment(install, config));
}

} // namespace

std::vector<std::string>
MaximaSession::launchCommand(const MaximaInstall &install) {
    const std::string evalExpr
        = "(progn (setf maxima::*prompt-prefix* \"" + std::string(kPromptPrefix)
          + "\") (setf maxima::*prompt-suffix* \"" + std::string(kPromptSuffix)
          + "\") (cl-user::run))";

    // Unquoted: quoting for the platform's argv parser is the transport's job.
    std::vector<std::string> argv{install.sbclExe.string(), "--core",
                                  install.maximaCore.string(), "--noinform"};

    if (install.raiseDynamicSpaceSize) {
        // What maxima.bat does on 64-bit builds, and for the same reason:
        // without the larger heap, load("lapack") runs out of dynamic space.
        argv.emplace_back("--dynamic-space-size");
        argv.emplace_back("2000");
    }

    argv.insert(argv.end(), {"--end-runtime-options", "--eval", evalExpr,
                             "--end-toplevel-options"});
    return argv;
}

std::vector<std::pair<std::string, std::string>>
MaximaSession::launchEnvironment(const MaximaInstall &install,
                                 const Config &config) {
    std::vector<std::pair<std::string, std::string>> env;

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
    readUntilPrompt(); // Consume the startup banner up to the first prompt.
    evaluate("display2d:false$");
}

std::string MaximaSession::evaluate(const std::string &statement) {
    writeLine(statement);
    const std::string raw = readUntilPrompt();

    // `raw` holds everything since the previous prompt: the (possibly empty)
    // result text, followed immediately by the next wrapped prompt
    // "<prefix>(%iN) <suffix>". Strip the prompt off the end.
    const size_t promptStart = raw.rfind(kPromptPrefix);
    std::string content
        = promptStart == std::string::npos ? raw : raw.substr(0, promptStart);
    content = trim(content);
    if (content.empty()) {
        return "";
    }

    static const std::regex resultLine(R"(\(%o\d+\)\s*([\s\S]*))");
    std::smatch match;
    if (std::regex_match(content, match, resultLine)) {
        return trim(match[1].str());
    }
    // No "(%oN)" label found (e.g. an error message) - return as-is so callers
    // can surface it.
    return content;
}

void MaximaSession::writeLine(const std::string &line) {
    transport_->send(line + "\n");
}

std::string MaximaSession::readUntilPrompt() {
    const auto deadline = std::chrono::steady_clock::now() + config_.timeout;

    std::string buffer;
    while (buffer.find(kPromptSuffix) == std::string::npos) {
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
    return buffer;
}

} // namespace mx::detail
