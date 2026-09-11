#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <filesystem>
#include <iostream>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

#include <symengine/expression.h>
#include <symengine/basic.h>
#include <symengine/derivative.h>
#include <symengine/simplify.h>
#include <symengine/parser.h>

using SymEngine::Expression;

// Root of the local Maxima installation. Adjust if Maxima is installed
// elsewhere.
static const std::string MAXIMA_ROOT = "C:\\maxima-5.50.0";

// Quotes a single argument for use in a Win32 CreateProcess command line,
// following the escaping rules understood by the standard MSVCRT argv
// parser (doubling backslashes that precede a quote, escaping embedded
// quotes). Unlike passing arguments through cmd.exe/_popen, CreateProcess
// takes one command-line string and hands it straight to the child's argv
// parser, so this is well-defined and avoids the batch-file quoting bugs
// we previously hit with `;` and nested quotes.
static std::string quoteArg(const std::string &arg) {
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

static std::string trim(const std::string &s) {
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
static std::filesystem::path findFile(const std::filesystem::path &root,
                                       const std::string &filename) {
    for (const auto &entry : std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied)) {
        if (entry.is_regular_file() && entry.path().filename() == filename) {
            return entry.path();
        }
    }
    throw std::runtime_error("Could not find " + filename + " under " + root.string());
}

// Drives a persistent Maxima session directly through its underlying SBCL
// Lisp image (bypassing maxima.bat/cmd.exe entirely), communicating over
// anonymous pipes. This avoids spawning a fresh Maxima process (and a
// flashing console window) per query, and avoids the batch-mode echo/marker
// parsing hacks: Maxima's *prompt-prefix*/*prompt-suffix* Lisp hooks (see
// doc/implementation/external-interface.txt) let us wrap every prompt in
// unambiguous, distinctive markers so results can be split out reliably.
class MaximaSession {
public:
    MaximaSession() {
        start();
    }

    ~MaximaSession() {
        stop();
    }

    MaximaSession(const MaximaSession &) = delete;
    MaximaSession &operator=(const MaximaSession &) = delete;

    // Sends one Maxima statement (must end in `;` or `$`) and returns the
    // text of its result line (e.g. "2*x*sin(x)+cos(x)*(2-x**2)"), or an
    // empty string if the statement produced no output (terminated in `$`).
    std::string evaluate(const std::string &statement) {
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

private:
    static constexpr const char *kPromptPrefix = "@MAXIMA_PROMPT_BEGIN@";
    static constexpr const char *kPromptSuffix = "@MAXIMA_PROMPT_END@";

    void start() {
        std::filesystem::path sbclExe = findFile(MAXIMA_ROOT, "sbcl.exe");
        std::filesystem::path coreFile = findFile(MAXIMA_ROOT, "maxima.core");

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
            throw std::runtime_error("Failed to create pipes for Maxima process");
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
            throw std::runtime_error("Failed to start Maxima (sbcl.exe)");
        }

        // Consume the startup banner up to the first prompt.
        readUntilPrompt();
        evaluate("display2d:false$");
    }

    void stop() {
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

    void writeLine(const std::string &line) {
        std::string withNewline = line + "\n";
        DWORD written = 0;
        WriteFile(stdinWrite_, withNewline.data(),
                   static_cast<DWORD>(withNewline.size()), &written, nullptr);
    }

    std::string readUntilPrompt() {
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

    HANDLE stdinWrite_ = nullptr;
    HANDLE stdoutRead_ = nullptr;
    PROCESS_INFORMATION procInfo_{};
};

// Asks Maxima to symbolically integrate `expr` with respect to `var`, and
// parses the resulting expression back into a SymEngine Expression.
static Expression integrateWithMaxima(MaximaSession &maxima, const std::string &expr,
                                      const std::string &var) {
    std::string result = maxima.evaluate("integrate(" + expr + ", " + var + ");");
    return Expression(SymEngine::parse(result));
}

int main() {
    // Build a symbolic expression: f(x) = x^2 + 3*x + 2
    Expression x("x");
    Expression f = pow(x, 2) + 3 * x + 2;

    std::cout << "f(x)        = " << f << std::endl;

    // Differentiate f with respect to x
    Expression df = f.diff(x);
    std::cout << "f'(x)       = " << df << std::endl;

    // Expand (x + 1)^3
    Expression expanded = expand(pow(x + 1, 3));
    std::cout << "(x+1)^3     = " << expanded << std::endl;

    // Substitute x = 5 into f(x)
    Expression f_at_5 = f.subs({{x, Expression(5)}});
    std::cout << "f(5)        = " << f_at_5 << std::endl;

    // Parse an expression from a string
    Expression g(SymEngine::parse("x**2 + 2*x*y + y**2"));
    std::cout << "g           = " << g << std::endl;

    Expression y("y");
    Expression g_at_1_2 = g.subs({{x, Expression(1)}, {y, Expression(2)}});
    std::cout << "g(x=1, y=2) = " << g_at_1_2 << std::endl;

    // SymEngine itself has no symbolic integrator, so shell out to a
    // persistent Maxima session (driven directly via its SBCL Lisp core)
    // for the integration and bring the result back into SymEngine.
    try {
        MaximaSession maxima;
        Expression integral = integrateWithMaxima(maxima, "x^2*sin(x)", "x");
        std::cout << "integral    = " << integral << std::endl;

        // Differentiating Maxima's antiderivative with SymEngine should
        // recover (a simplified form of) the original integrand.
        Expression check = expand(integral.diff(x));
        std::cout << "check       = " << check << std::endl;
    } catch (const std::exception &e) {
        std::cerr << "Maxima call failed: " << e.what() << std::endl;
    }

    return 0;
}
