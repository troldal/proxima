#include "kernel/session.hpp"

#include <mx/errors.hpp>

#include <filesystem>
#include <regex>
#include <vector>

namespace mx::detail {
namespace {

// Quotes a single argument for use in a Win32 CreateProcess command line,
// following the escaping rules understood by the standard MSVCRT argv
// parser (doubling backslashes that precede a quote, escaping embedded
// quotes). Unlike passing arguments through cmd.exe/_popen, CreateProcess
// takes one command-line string and hands it straight to the child's argv
// parser, so this is well-defined and avoids the batch-file quoting bugs
// we previously hit with `;` and nested quotes.
std::string quoteArg(const std::string &arg) {
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) {
        return arg;
    }
    std::string result = "\"";
    for (auto it = arg.begin();; ++it) {
        size_t backslashes = 0;
        while (it != arg.end() && *it == '\\') {
            ++backslashes;
            ++it;
        }
        if (it == arg.end()) {
            result.append(backslashes * 2, '\\');
            break;
        }
        if (*it == '"') {
            result.append(backslashes * 2 + 1, '\\');
            result.push_back('"');
        } else {
            result.append(backslashes, '\\');
            result.push_back(*it);
        }
    }
    result.push_back('"');
    return result;
}

std::string trim(const std::string &s) {
    size_t first = s.find_first_not_of(" \t\r\n");
    size_t last = s.find_last_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    return s.substr(first, last - first + 1);
}

// Recursively searches `root` for a file named `filename` and returns the
// first match. Used to locate sbcl.exe and maxima.core without hard-coding
// the version-specific "binary-sbcl" subdirectory name.
//
// PLAN.md step 5 replaces this with a targeted lookup: the core actually
// lives at <root>/lib/maxima/<tag>/binary-sbcl/maxima.core, so this walk
// needlessly descends into gnuplot, vtk, clisp and doc.
std::filesystem::path findFile(const std::filesystem::path &root,
                               const std::string &filename) {
    for (const auto &entry : std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied)) {
        if (entry.is_regular_file() && entry.path().filename() == filename) {
            return entry.path();
        }
    }
    throw KernelError("Could not find " + filename + " under " + root.string());
}

} // namespace

MaximaSession::MaximaSession(Config config) : config_(std::move(config)) {
    start();
}

MaximaSession::~MaximaSession() {
    stop();
}

std::string MaximaSession::evaluate(const std::string &statement) {
    writeLine(statement);
    std::string raw = readUntilPrompt();

    // `raw` holds everything since the previous prompt: the (possibly
    // empty) result text, followed immediately by the next wrapped
    // prompt "<prefix>(%iN) <suffix>". Strip the prompt off the end.
    size_t promptStart = raw.rfind(kPromptPrefix);
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
    // No "(%oN)" label found (e.g. an error message) - return as-is so
    // callers can surface it.
    return content;
}

void MaximaSession::start() {
    std::filesystem::path sbclExe = findFile(config_.maximaRoot, "sbcl.exe");
    std::filesystem::path coreFile = findFile(config_.maximaRoot, "maxima.core");

    const std::string evalExpr
        = "(progn (setf maxima::*prompt-prefix* \"" + std::string(kPromptPrefix)
          + "\") (setf maxima::*prompt-suffix* \"" + std::string(kPromptSuffix)
          + "\") (cl-user::run))";

    std::string commandLine = quoteArg(sbclExe.string()) + " --core "
                              + quoteArg(coreFile.string())
                              + " --noinform --end-runtime-options --eval "
                              + quoteArg(evalExpr) + " --end-toplevel-options";

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE childStdinRead = nullptr, childStdoutWrite = nullptr;
    if (!CreatePipe(&childStdinRead, &stdinWrite_, &sa, 0)
        || !CreatePipe(&stdoutRead_, &childStdoutWrite, &sa, 0)) {
        throw KernelError("Failed to create pipes for Maxima process");
    }
    // The ends we keep must not be inherited by the child.
    SetHandleInformation(stdinWrite_, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stdoutRead_, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = childStdinRead;
    si.hStdOutput = childStdoutWrite;
    si.hStdError = childStdoutWrite;

    std::vector<char> cmdBuf(commandLine.begin(), commandLine.end());
    cmdBuf.push_back('\0');

    BOOL ok = CreateProcessA(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &procInfo_);

    CloseHandle(childStdinRead);
    CloseHandle(childStdoutWrite);

    if (!ok) {
        CloseHandle(stdinWrite_);
        CloseHandle(stdoutRead_);
        throw KernelError("Failed to start Maxima (sbcl.exe)");
    }

    // Consume the startup banner up to the first prompt.
    readUntilPrompt();
    evaluate("display2d:false$");
}

void MaximaSession::stop() {
    if (procInfo_.hProcess) {
        writeLine("quit();");
        WaitForSingleObject(procInfo_.hProcess, 2000);
        TerminateProcess(procInfo_.hProcess, 0);
        CloseHandle(procInfo_.hProcess);
        CloseHandle(procInfo_.hThread);
    }
    if (stdinWrite_) {
        CloseHandle(stdinWrite_);
    }
    if (stdoutRead_) {
        CloseHandle(stdoutRead_);
    }
}

void MaximaSession::writeLine(const std::string &line) {
    std::string withNewline = line + "\n";
    DWORD written = 0;
    WriteFile(stdinWrite_, withNewline.data(),
              static_cast<DWORD>(withNewline.size()), &written, nullptr);
}

std::string MaximaSession::readUntilPrompt() {
    std::string buffer;
    char chunk[4096];
    while (buffer.find(kPromptSuffix) == std::string::npos) {
        DWORD bytesRead = 0;
        if (!ReadFile(stdoutRead_, chunk, sizeof(chunk), &bytesRead, nullptr)
            || bytesRead == 0) {
            break; // Pipe closed / process exited.
        }
        buffer.append(chunk, bytesRead);
    }
    return buffer;
}

} // namespace mx::detail
