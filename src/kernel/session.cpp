#include "kernel/session.hpp"

#include "transport/child_process_win32.hpp"

#include <mx/errors.hpp>

#include <chrono>
#include <filesystem>
#include <regex>

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

// Recursively searches `root` for a file named `filename` and returns the
// first match. Used to locate sbcl.exe and maxima.core without hard-coding the
// version-specific "binary-sbcl" subdirectory name.
//
// PLAN.md step 5 replaces this with a targeted lookup: the core actually lives
// at <root>/lib/maxima/<tag>/binary-sbcl/maxima.core, so this walk needlessly
// descends into gnuplot, vtk, clisp and doc, and could match the wrong
// sbcl.exe.
std::filesystem::path findFile(const std::filesystem::path &root,
                               const std::string &filename) {
    if (!std::filesystem::is_directory(root)) {
        throw KernelError("Maxima root is not a directory: " + root.string());
    }
    for (const auto &entry : std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied)) {
        if (entry.is_regular_file() && entry.path().filename() == filename) {
            return entry.path();
        }
    }
    throw KernelError("Could not find " + filename + " under " + root.string());
}

// How long to wait for any single chunk of output before checking whether the
// child is still alive. Not a deadline; see readUntilPrompt.
constexpr std::chrono::milliseconds kPollInterval{50};

} // namespace

std::vector<std::string> MaximaSession::launchCommand(const Config &config) {
    const std::filesystem::path sbclExe = findFile(config.maximaRoot, "sbcl.exe");
    const std::filesystem::path coreFile
        = findFile(config.maximaRoot, "maxima.core");

    const std::string evalExpr
        = "(progn (setf maxima::*prompt-prefix* \"" + std::string(kPromptPrefix)
          + "\") (setf maxima::*prompt-suffix* \"" + std::string(kPromptSuffix)
          + "\") (cl-user::run))";

    // Unquoted: quoting for the platform's argv parser is the transport's job.
    return {
        sbclExe.string(),
        "--core", coreFile.string(),
        "--noinform",
        "--end-runtime-options",
        "--eval", evalExpr,
        "--end-toplevel-options",
    };
}

MaximaSession::MaximaSession(Config config)
    : config_(std::move(config)),
      transport_(
          std::make_unique<ChildProcessTransport>(launchCommand(config_))) {
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
