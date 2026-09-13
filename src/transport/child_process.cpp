#include "transport/child_process.hpp"

#include "transport/process_env.hpp"
#include "util/utf8.hpp"

#include <mx/errors.hpp>

#include <boost/asio/buffer.hpp>
#include <boost/asio/connect_pipe.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/readable_pipe.hpp>
#include <boost/asio/writable_pipe.hpp>
#include <boost/asio/write.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/stdio.hpp>
#include <boost/process/v2/environment.hpp>
#if defined(_WIN32)
#include <boost/process/v2/windows/creation_flags.hpp>
#else
#include <fcntl.h>
#endif

#include <array>
#include <chrono>
#include <optional>
#include <thread>

// One implementation for every platform, over Boost.Process v2 and Boost.Asio.
//
// This replaces a Win32 file and a POSIX file that between them did by hand
// what Boost.Process does properly: CreateProcess and fork/exec, argument
// quoting, a close-on-exec pipe to report a failed exec, and a read with a
// timeout. The hand-written Windows read had to poll — PeekNamedPipe, then
// Sleep(1) — because a blocking ReadFile on an anonymous pipe cannot be
// abandoned, and Sleep(1) is a whole scheduler tick, so every round trip to
// Maxima cost at least 15 ms however little it said. Asio's pipes are
// overlapped on Windows, so a read waits on the completion port and returns
// the moment bytes arrive.

namespace mx::detail {
namespace {

namespace asio = boost::asio;
namespace bp = boost::process::v2;

/// How long a child that has been told to go is given to leave on its own —
/// to flush, and to exit cleanly — before it is terminated.
constexpr std::chrono::seconds kExitGrace{2};

/// Closes a raw pipe handle when it goes out of scope.
struct PipeEndCloser {
    asio::detail::native_pipe_handle handle;
    ~PipeEndCloser() { asio::detail::close_pipe(handle); }
};

} // namespace

struct ChildProcessTransport::Impl {
    // Declared first, so it is destroyed last: the pipes and the process are
    // all bound to it.
    asio::io_context context;

    /// The parent's end of the child's standard input.
    asio::writable_pipe input{context};

    /// The parent's end of the pipe the child's standard output *and* standard
    /// error both write to, so Maxima's diagnostics arrive in the same stream
    /// as its results.
    asio::readable_pipe output{context};

    std::optional<bp::process> process;

    /// Set once the output has reached end of file or failed, which is what a
    /// child that has exited looks like from here.
    bool closed = false;
};

ChildProcessTransport::ChildProcessTransport(const std::vector<std::string> &argv,
                                             const std::vector<EnvOverride> &env)
    : impl_(std::make_unique<Impl>()) {
    if (argv.empty()) {
        throw KernelError("ChildProcessTransport requires at least an executable");
    }

    // stdout and stderr share one pipe. Boost.Process makes a fresh pipe for
    // every stream it is handed a pipe object for, which would give them one
    // each, so this one is made here and the child is given the same write end
    // for both.
    //
    // It is made from raw handles, and only the parent's read end is bound to
    // the io_context — deliberately not with asio::connect_pipe, which binds
    // both ends. On Windows, binding a handle associates its *file object* with
    // the io_context's completion port, and a handle the child inherits is the
    // same file object. SBCL writes to its standard output with overlapped I/O,
    // so each of its writes would post a completion to *this* process's port,
    // carrying an OVERLAPPED address from SBCL's address space, which Asio would
    // take for one of its own operations and call. The first version of this
    // file did exactly that, and it corrupted memory during Maxima's startup:
    // on Windows only, only with Maxima (cmd.exe writes without OVERLAPPED, so
    // the transport's own tests passed), and never on Linux, which has no
    // completion ports. Boost.Process's own stdio bindings keep the child's end
    // unbound for the same reason.
    asio::detail::native_pipe_handle ends[2];
    boost::system::error_code ec;
    asio::detail::create_pipe(ends, ec);
    if (ec) {
        throw KernelError("Failed to create the output pipe for a child process: "
                          + ec.message());
    }

    // The child's end is closed however this constructor leaves. Once the child
    // is running it holds its own copy, and keeping the parent's open would stop
    // the pipe ever reporting end of file: a dead child would look like a silent
    // one.
    const PipeEndCloser childEnd{ends[1]};

#if !defined(_WIN32)
    // Keep the parent's end out of the child, as Boost.Process's own binding
    // does. (On Windows it is not inheritable to begin with.)
    if (::fcntl(ends[0], F_SETFD, FD_CLOEXEC) == -1) {
        asio::detail::close_pipe(ends[0]);
        throw KernelError("Failed to configure the output pipe for a child process");
    }
#endif

    impl_->output.assign(ends[0], ec);
    if (ec) {
        asio::detail::close_pipe(ends[0]);
        throw KernelError("Failed to bind the output pipe for a child process: "
                          + ec.message());
    }

    // Merged over the inherited environment rather than replacing it, so the
    // child keeps PATH and the rest. Must outlive the launch: on POSIX the
    // launcher points straight into these strings.
    const std::vector<std::string> environment = mergeEnvironment(env);
    const std::vector<std::string> arguments(argv.begin() + 1, argv.end());

    // The arguments and the environment are UTF-8, and Boost.Process converts
    // them as such; the executable has to be read the same way. Constructing
    // the path straight from the std::string would read it as the ANSI code
    // page on Windows and launch a different — most likely no — file.
    const auto executable = tryPathFromUtf8(argv.front());
    if (!executable) {
        throw KernelError("Failed to start child process: the executable path "
                          "is not valid UTF-8");
    }

    try {
        impl_->process.emplace(
            impl_->context.get_executor(), *executable,
            arguments,
            // The same write end twice. On Windows it then appears twice,
            // consecutively, in the list of handles the child inherits; the
            // launcher drops adjacent duplicates before CreateProcessW sees the
            // list, which is why the order in, out, err matters here.
            bp::process_stdio{impl_->input, ends[1], ends[1]},
            bp::process_environment(environment)
#if defined(_WIN32)
            // No console window: a library should not flash one up each time
            // it starts a kernel.
            , bp::windows::process_creation_flags<CREATE_NO_WINDOW>()
#endif
        );
    } catch (const boost::system::system_error &error) {
        // Reported synchronously on both platforms. On POSIX Boost.Process
        // carries a failed exec back to the parent, so a missing executable is
        // an exception here, not a child that exits with 127 later.
        throw KernelError("Failed to start child process: " + argv.front() + ": "
                          + error.what());
    }
}

ChildProcessTransport::~ChildProcessTransport() {
    kill();
}

void ChildProcessTransport::send(std::string_view bytes) {
    if (impl_->closed || !impl_->input.is_open()) {
        return;
    }
    boost::system::error_code ec;
    asio::write(impl_->input, asio::buffer(bytes.data(), bytes.size()), ec);
    if (ec) {
        // The child has gone. The hand-written transports dropped this on the
        // floor, so the failure only surfaced as a timeout on the reply;
        // marking the transport closed makes the next receive() and alive()
        // report it at once.
        impl_->closed = true;
    }
}

std::string ChildProcessTransport::receive(std::chrono::milliseconds timeout) {
    if (impl_->closed || !impl_->output.is_open()) {
        return {};
    }

    std::array<char, 4096> chunk{};
    std::size_t received = 0;
    boost::system::error_code result;
    bool finished = false;

    impl_->output.async_read_some(
        asio::buffer(chunk),
        [&](const boost::system::error_code &ec, std::size_t count) {
            result = ec;
            received = count;
            finished = true;
        });

    // Runs until the read completes or the timeout passes, whichever is first.
    impl_->context.restart();
    impl_->context.run_for(timeout);

    if (!finished) {
        // Nothing arrived in time. Cancel the read and let its handler run
        // now, so no operation is left pending into the next call — the
        // handler refers to this call's locals. A read that completed in the
        // meantime still delivers its bytes.
        boost::system::error_code ignored;
        impl_->output.cancel(ignored);
        impl_->context.restart();
        impl_->context.run();
    }

    if (result && result != asio::error::operation_aborted) {
        // End of file, or a broken pipe: either way the child has exited.
        impl_->closed = true;
    }
    return std::string(chunk.data(), received);
}

bool ChildProcessTransport::alive() const {
    if (impl_->closed || !impl_->process) {
        return false;
    }
    // An error here means the process can no longer be asked about — already
    // reaped, say — which is as good as not running.
    boost::system::error_code ec;
    const bool running = impl_->process->running(ec);
    return running && !ec;
}

void ChildProcessTransport::kill() {
    boost::system::error_code ignored;

    // Closing stdin gives a child that was asked to quit a chance to see end
    // of input and leave on its own.
    impl_->input.close(ignored);

    if (impl_->process) {
        const auto deadline = std::chrono::steady_clock::now() + kExitGrace;
        for (;;) {
            boost::system::error_code ec;
            const bool running = impl_->process->running(ec);
            if (!running || ec) {
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                impl_->process->terminate(ignored);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // The process object would terminate a running child when destroyed
        // anyway; by now there is none.
        impl_->process.reset();
    }

    impl_->output.close(ignored);
    impl_->closed = true;
}

} // namespace mx::detail
