// The child process transport: environment merging, launching, and a few real
// children built from what every machine already has — cmd.exe on Windows,
// /bin/sh elsewhere. No Maxima.
//
// Argument quoting on Windows used to be unit-tested here, against a
// hand-written quoter. Boost.Process does that now, and the real test of it is
// the Maxima launch in the integration suite, which passes SBCL a multi-line
// Lisp form full of quotes.

#include <doctest/doctest.h>

#include "transport/child_process.hpp"
#include "transport/process_env.hpp"

#include <mx/errors.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

using mx::detail::ChildProcessTransport;
using mx::detail::mergeEnvironment;
using namespace std::chrono_literals;

namespace {

bool contains(const std::vector<std::string> &entries, const std::string &wanted) {
    return std::find(entries.begin(), entries.end(), wanted) != entries.end();
}

size_t countWithName(const std::vector<std::string> &entries,
                     const std::string &name) {
    const std::string prefix = name + "=";
    return static_cast<size_t>(
        std::count_if(entries.begin(), entries.end(),
                      [&prefix](const std::string &entry) {
                          return entry.rfind(prefix, 0) == 0;
                      }));
}

/// Reads until `token` has arrived, the child has gone, or `limit` passes, and
/// returns everything read.
std::string readUntil(ChildProcessTransport &child, std::string_view token,
                      std::chrono::milliseconds limit = 10s) {
    std::string seen;
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (seen.find(token) == std::string::npos
           && std::chrono::steady_clock::now() < deadline) {
        const std::string chunk = child.receive(100ms);
        seen += chunk;
        if (chunk.empty() && !child.alive()) {
            break;
        }
    }
    return seen;
}

#ifdef _WIN32
std::string commandShell() {
    const char *root = std::getenv("SystemRoot");
    return std::string(root != nullptr ? root : "C:\\Windows")
           + "\\System32\\cmd.exe";
}
#endif

} // namespace

// --- environment merging ------------------------------------------------------

TEST_CASE("an override that matches nothing is appended") {
    const auto entries = mergeEnvironment({{"MX_TEST_NEW_VAR", "hello"}});
    CHECK(contains(entries, "MX_TEST_NEW_VAR=hello"));
    CHECK(countWithName(entries, "MX_TEST_NEW_VAR") == 1);
}

TEST_CASE("the inherited environment is kept") {
    // A child that loses PATH cannot resolve its own shared libraries, so this
    // has to be a merge rather than a replacement.
    const auto entries = mergeEnvironment({{"MX_TEST_X", "1"}});
    CHECK(entries.size() > 1);
    const bool keptPath
        = std::any_of(entries.begin(), entries.end(), [](const std::string &e) {
              return e.rfind("PATH=", 0) == 0 || e.rfind("Path=", 0) == 0;
          });
    CHECK(keptPath);
}

TEST_CASE("merging with no overrides reproduces the environment") {
    const auto plain = mergeEnvironment({});
    const auto withOne = mergeEnvironment({{"MX_TEST_ONLY", "1"}});
    CHECK(withOne.size() == plain.size() + 1);
}

TEST_CASE("name matching follows the platform's own rules") {
    // Windows compares variable names case-insensitively, so an override
    // spelled differently from the inherited entry must replace it. POSIX
    // treats the two spellings as different variables, so it must not.
    const auto entries = mergeEnvironment({{"path", "/only"}});

#ifdef _WIN32
    // Exactly one PATH-ish entry survives, and it is ours.
    const auto isPath = [](const std::string &entry) {
        const std::string lowered = [&entry] {
            std::string copy = entry.substr(0, entry.find('='));
            std::transform(copy.begin(), copy.end(), copy.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            return copy;
        }();
        return lowered == "path";
    };
    CHECK(std::count_if(entries.begin(), entries.end(), isPath) == 1);
    CHECK(contains(entries, "path=/only"));
#else
    // Lowercase "path" is a new variable; the real PATH is untouched.
    CHECK(contains(entries, "path=/only"));
    CHECK(countWithName(entries, "PATH") == 1);
#endif
}

// --- launching ------------------------------------------------------------------

TEST_CASE("launching a nonexistent executable throws rather than returning") {
    // Both platforms report this synchronously, as an exception, rather than
    // producing a transport that looks alive.
    CHECK_THROWS_AS(ChildProcessTransport({"no_such_program_xyz_12345"}),
                    mx::KernelError);
    CHECK_THROWS_AS(ChildProcessTransport({}), mx::KernelError);
}

TEST_CASE("what a real child writes comes back") {
    // The command is written so the token appears in the output only if the
    // command actually ran: cmd.exe drops the caret and the shell the quotes,
    // so an echo of the input itself would not match.
#ifdef _WIN32
    ChildProcessTransport child({commandShell(), "/q"});
    child.send("echo mx_transport^_ok\r\n");
#else
    ChildProcessTransport child({"/bin/sh"});
    child.send("echo mx_transport'_'ok\n");
#endif
    CHECK(readUntil(child, "mx_transport_ok").find("mx_transport_ok")
          != std::string::npos);
    CHECK(child.alive());

    child.kill();
    CHECK_FALSE(child.alive());
}

TEST_CASE("a silent child makes receive wait out its timeout") {
    // This is the path the Boost.Process transport changed most. The old
    // Windows transport polled with Sleep(1); this one waits on the pipe and
    // must still honour the caller's deadline, returning empty — not an error,
    // and not a dead child.
#ifdef _WIN32
    ChildProcessTransport child(
        {commandShell(), "/c", "ping -n 30 127.0.0.1 >nul"});
#else
    ChildProcessTransport child({"/bin/sh", "-c", "sleep 30"});
#endif

    const auto start = std::chrono::steady_clock::now();
    const std::string nothing = child.receive(200ms);
    const auto waited = std::chrono::steady_clock::now() - start;

    CHECK(nothing.empty());
    CHECK(waited >= 150ms);
    CHECK(child.alive());

    SUBCASE("and a child that ignores end of input is still ended") {
        // Neither ping nor sleep reads stdin, so closing it does not make them
        // leave: kill() has to terminate them after its grace period.
        const auto killStart = std::chrono::steady_clock::now();
        child.kill();
        CHECK(std::chrono::steady_clock::now() - killStart < 10s);
        CHECK_FALSE(child.alive());
    }
}

TEST_CASE("a slow child: many empty reads, then more than one read's worth") {
    // What a Maxima session looks like at startup: nothing for a while, which
    // the session waits out in short reads, then a burst larger than a single
    // read. Accumulated the way MaximaSession::readFrame accumulates, into a
    // string that lives across every call.
    //
    // It cannot catch everything that goes wrong at that point. cmd.exe and sh
    // write without overlapped I/O, so the Windows completion-port corruption
    // described in child_process.cpp never shows here; it only ever showed with
    // SBCL, and the Maxima integration suite is what guards against it.
#ifdef _WIN32
    ChildProcessTransport child(
        {commandShell(), "/c",
         "ping -n 2 127.0.0.1 >nul & dir /s %SystemRoot%\\System32\\drivers"});
#else
    ChildProcessTransport child({"/bin/sh", "-c", "sleep 1; ls -la /usr/bin"});
#endif

    std::string output;
    int emptyReads = 0;
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (std::chrono::steady_clock::now() < deadline) {
        const std::string chunk = child.receive(50ms);
        if (chunk.empty()) {
            if (!child.alive()) {
                break;
            }
            ++emptyReads;
            continue;
        }
        output += chunk;
    }

    CHECK(emptyReads >= 5);
    CHECK(output.size() > 4096);
}

TEST_CASE("an environment override reaches the child") {
#ifdef _WIN32
    ChildProcessTransport child({commandShell(), "/c", "echo %MX_TRANSPORT_TEST%"},
                                {{"MX_TRANSPORT_TEST", "value42"}});
#else
    ChildProcessTransport child({"/bin/sh", "-c", "echo \"$MX_TRANSPORT_TEST\""},
                                {{"MX_TRANSPORT_TEST", "value42"}});
#endif
    // Without the override the output would be the unexpanded name, or empty.
    CHECK(readUntil(child, "value42").find("value42") != std::string::npos);
}

#ifndef _WIN32
TEST_CASE("arguments reach the child exactly as given") {
    // Spaces, quotes and a newline, one argument each. The shell prints each
    // argument it received in brackets.
    ChildProcessTransport child({"/bin/sh", "-c", "printf '[%s]' \"$@\"", "sh",
                                 "a b", "q\"x", "line\nbreak"});
    CHECK(readUntil(child, "[line\nbreak]").find("[a b][q\"x][line\nbreak]")
          != std::string::npos);
}
#endif
