#include "transport/child_process.hpp"

#include "transport/win32_process_utils.hpp"

#include <mx/errors.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

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

std::string buildCommandLine(const std::vector<std::string> &argv) {
    std::string commandLine;
    for (const std::string &arg : argv) {
        if (!commandLine.empty()) {
            commandLine.push_back(' ');
        }
        commandLine += quoteArg(arg);
    }
    return commandLine;
}

std::vector<char>
buildEnvironmentBlock(const std::vector<EnvOverride> &overrides) {
    std::vector<char> block;
    for (const std::string &entry : mergeEnvironment(overrides)) {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back('\0');
    }
    block.push_back('\0'); // Blocks are terminated by a second NUL.
    return block;
}

struct ChildProcessTransport::Impl {
    HANDLE stdinWrite = nullptr;
    HANDLE stdoutRead = nullptr;
    PROCESS_INFORMATION procInfo{};
    bool closed = false;
};

ChildProcessTransport::ChildProcessTransport(const std::vector<std::string> &argv,
                                             const std::vector<EnvOverride> &env)
    : impl_(std::make_unique<Impl>()) {
    if (argv.empty()) {
        throw KernelError("ChildProcessTransport requires at least an executable");
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE childStdinRead = nullptr, childStdoutWrite = nullptr;
    if (!CreatePipe(&childStdinRead, &impl_->stdinWrite, &sa, 0)
        || !CreatePipe(&impl_->stdoutRead, &childStdoutWrite, &sa, 0)) {
        throw KernelError("Failed to create pipes for child process");
    }
    // The ends we keep must not be inherited by the child.
    SetHandleInformation(impl_->stdinWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(impl_->stdoutRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = childStdinRead;
    si.hStdOutput = childStdoutWrite;
    si.hStdError = childStdoutWrite;

    const std::string commandLine = buildCommandLine(argv);
    std::vector<char> cmdBuf(commandLine.begin(), commandLine.end());
    cmdBuf.push_back('\0');

    // An empty override list means "inherit everything", which CreateProcess
    // spells as a null block rather than an empty one.
    std::vector<char> envBlock;
    if (!env.empty()) {
        envBlock = buildEnvironmentBlock(env);
    }

    const BOOL ok = CreateProcessA(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW,
                                   envBlock.empty() ? nullptr : envBlock.data(),
                                   nullptr, &si, &impl_->procInfo);

    // The child owns its ends now; holding them open would keep the pipe from
    // ever reporting end-of-stream.
    CloseHandle(childStdinRead);
    CloseHandle(childStdoutWrite);

    if (!ok) {
        CloseHandle(impl_->stdinWrite);
        CloseHandle(impl_->stdoutRead);
        impl_->stdinWrite = impl_->stdoutRead = nullptr;
        throw KernelError("Failed to start child process: " + argv.front());
    }
}

ChildProcessTransport::~ChildProcessTransport() {
    kill();
}

void ChildProcessTransport::send(std::string_view bytes) {
    if (impl_->closed || !impl_->stdinWrite) {
        return;
    }
    DWORD written = 0;
    WriteFile(impl_->stdinWrite, bytes.data(), static_cast<DWORD>(bytes.size()),
              &written, nullptr);
}

std::string ChildProcessTransport::receive(std::chrono::milliseconds timeout) {
    if (impl_->closed || !impl_->stdoutRead) {
        return {};
    }

    // PeekNamedPipe rather than a bare blocking ReadFile: a blocking read on an
    // anonymous pipe cannot be abandoned, so honouring the caller's deadline
    // means polling for availability. The POSIX transport needs no equivalent —
    // poll() takes the deadline directly. Step 13 replaces this with a
    // dedicated reader thread feeding a bounded queue.
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(impl_->stdoutRead, nullptr, 0, nullptr, &available,
                           nullptr)) {
            impl_->closed = true; // Write end gone: the child has exited.
            return {};
        }

        if (available > 0) {
            char chunk[4096];
            const DWORD wanted
                = std::min<DWORD>(available, static_cast<DWORD>(sizeof(chunk)));
            DWORD bytesRead = 0;
            if (!ReadFile(impl_->stdoutRead, chunk, wanted, &bytesRead, nullptr)
                || bytesRead == 0) {
                impl_->closed = true;
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
    if (impl_->closed || !impl_->procInfo.hProcess) {
        return false;
    }
    return WaitForSingleObject(impl_->procInfo.hProcess, 0) == WAIT_TIMEOUT;
}

void ChildProcessTransport::kill() {
    if (impl_->procInfo.hProcess) {
        // Give a child that has already been asked to quit a moment to leave on
        // its own, so it can flush and clean up; terminate only if it does not.
        if (WaitForSingleObject(impl_->procInfo.hProcess, 2000) != WAIT_OBJECT_0) {
            TerminateProcess(impl_->procInfo.hProcess, 0);
        }
        CloseHandle(impl_->procInfo.hProcess);
        CloseHandle(impl_->procInfo.hThread);
        impl_->procInfo = PROCESS_INFORMATION{};
    }
    if (impl_->stdinWrite) {
        CloseHandle(impl_->stdinWrite);
        impl_->stdinWrite = nullptr;
    }
    if (impl_->stdoutRead) {
        CloseHandle(impl_->stdoutRead);
        impl_->stdoutRead = nullptr;
    }
    impl_->closed = true;
}

} // namespace mx::detail
