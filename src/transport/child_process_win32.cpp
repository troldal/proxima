#include "transport/child_process_win32.hpp"

#include <mx/errors.hpp>

#include <algorithm>

namespace mx::detail {

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

ChildProcessTransport::ChildProcessTransport(const std::vector<std::string> &argv) {
    if (argv.empty()) {
        throw KernelError("ChildProcessTransport requires at least an executable");
    }

    std::string commandLine;
    for (const std::string &arg : argv) {
        if (!commandLine.empty()) {
            commandLine.push_back(' ');
        }
        commandLine += quoteArg(arg);
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE childStdinRead = nullptr, childStdoutWrite = nullptr;
    if (!CreatePipe(&childStdinRead, &stdinWrite_, &sa, 0)
        || !CreatePipe(&stdoutRead_, &childStdoutWrite, &sa, 0)) {
        throw KernelError("Failed to create pipes for child process");
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

    // The child owns its ends now; holding them open would keep the pipe from
    // ever reporting end-of-stream.
    CloseHandle(childStdinRead);
    CloseHandle(childStdoutWrite);

    if (!ok) {
        CloseHandle(stdinWrite_);
        CloseHandle(stdoutRead_);
        stdinWrite_ = stdoutRead_ = nullptr;
        throw KernelError("Failed to start child process: " + argv.front());
    }
}

ChildProcessTransport::~ChildProcessTransport() {
    kill();
}

void ChildProcessTransport::send(std::string_view bytes) {
    if (closed_ || !stdinWrite_) {
        return;
    }
    DWORD written = 0;
    WriteFile(stdinWrite_, bytes.data(), static_cast<DWORD>(bytes.size()),
              &written, nullptr);
}

std::string ChildProcessTransport::receive(std::chrono::milliseconds timeout) {
    if (closed_ || !stdoutRead_) {
        return {};
    }

    // PeekNamedPipe rather than a bare blocking ReadFile: a blocking read on an
    // anonymous pipe cannot be abandoned, so honouring the caller's deadline
    // means polling for availability. Step 13 replaces this with a dedicated
    // reader thread feeding a bounded queue.
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(stdoutRead_, nullptr, 0, nullptr, &available, nullptr)) {
            closed_ = true; // Write end gone: the child has exited.
            return {};
        }

        if (available > 0) {
            char chunk[4096];
            const DWORD wanted
                = std::min<DWORD>(available, static_cast<DWORD>(sizeof(chunk)));
            DWORD bytesRead = 0;
            if (!ReadFile(stdoutRead_, chunk, wanted, &bytesRead, nullptr)
                || bytesRead == 0) {
                closed_ = true;
                return {};
            }
            return std::string(chunk, bytesRead);
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            return {}; // Timed out with nothing available; alive() stays true.
        }
        Sleep(1);
    }
}

bool ChildProcessTransport::alive() const {
    if (closed_ || !procInfo_.hProcess) {
        return false;
    }
    return WaitForSingleObject(procInfo_.hProcess, 0) == WAIT_TIMEOUT;
}

void ChildProcessTransport::kill() {
    if (procInfo_.hProcess) {
        // Give a child that has already been asked to quit a moment to leave on
        // its own, so it can flush and clean up; terminate only if it does not.
        if (WaitForSingleObject(procInfo_.hProcess, 2000) != WAIT_OBJECT_0) {
            TerminateProcess(procInfo_.hProcess, 0);
        }
        CloseHandle(procInfo_.hProcess);
        CloseHandle(procInfo_.hThread);
        procInfo_ = PROCESS_INFORMATION{};
    }
    if (stdinWrite_) {
        CloseHandle(stdinWrite_);
        stdinWrite_ = nullptr;
    }
    if (stdoutRead_) {
        CloseHandle(stdoutRead_);
        stdoutRead_ = nullptr;
    }
    closed_ = true;
}

} // namespace mx::detail
