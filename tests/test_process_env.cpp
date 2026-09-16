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
#include "util/utf8.hpp"

#include <proxima/errors.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

using proxima::detail::ChildProcessTransport;
using proxima::detail::merge_environment;
using namespace std::chrono_literals;

namespace {

bool contains(const std::vector<std::string> &entries, const std::string &wanted) {
    return std::find(entries.begin(), entries.end(), wanted) != entries.end();
}

size_t count_with_name(const std::vector<std::string> &entries,
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
std::string read_until(ChildProcessTransport &child, std::string_view token,
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
/// cmd.exe, in UTF-8 like every string the transport takes. SystemRoot is read
/// through merge_environment rather than std::getenv, which on Windows gives the
/// ANSI code page's copy of the environment.
std::string command_shell() {
    std::string root = "C:\\Windows";
    for (const std::string &entry : merge_environment({})) {
        const std::string name = entry.substr(0, entry.find('='));
        if (name.size() == 10
            && std::equal(name.begin(), name.end(), "SYSTEMROOT",
                          [](char a, char b) {
                              return std::toupper(static_cast<unsigned char>(a))
                                     == b;
                          })) {
            root = entry.substr(name.size() + 1);
            break;
        }
    }
    return root + "\\System32\\cmd.exe";
}
#endif

} // namespace

// --- environment merging ------------------------------------------------------

TEST_CASE("an override that matches nothing is appended") {
    const auto entries = merge_environment({{"PROXIMA_TEST_NEW_VAR", "hello"}});
    CHECK(contains(entries, "PROXIMA_TEST_NEW_VAR=hello"));
    CHECK(count_with_name(entries, "PROXIMA_TEST_NEW_VAR") == 1);
}

TEST_CASE("the inherited environment is kept") {
    // A child that loses PATH cannot resolve its own shared libraries, so this
    // has to be a merge rather than a replacement.
    const auto entries = merge_environment({{"PROXIMA_TEST_X", "1"}});
    CHECK(entries.size() > 1);
    const bool kept_path
        = std::any_of(entries.begin(), entries.end(), [](const std::string &e) {
              return e.rfind("PATH=", 0) == 0 || e.rfind("Path=", 0) == 0;
          });
    CHECK(kept_path);
}

TEST_CASE("merging with no overrides reproduces the environment") {
    const auto plain = merge_environment({});
    const auto with_one = merge_environment({{"PROXIMA_TEST_ONLY", "1"}});
    CHECK(with_one.size() == plain.size() + 1);
}

TEST_CASE("name matching follows the platform's own rules") {
    // Windows compares variable names case-insensitively, so an override
    // spelled differently from the inherited entry must replace it. POSIX
    // treats the two spellings as different variables, so it must not.
    const auto entries = merge_environment({{"path", "/only"}});

#ifdef _WIN32
    // Exactly one PATH-ish entry survives, and it is ours.
    const auto is_path = [](const std::string &entry) {
        const std::string lowered = [&entry] {
            std::string copy = entry.substr(0, entry.find('='));
            std::transform(copy.begin(), copy.end(), copy.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            return copy;
        }();
        return lowered == "path";
    };
    CHECK(std::count_if(entries.begin(), entries.end(), is_path) == 1);
    CHECK(contains(entries, "path=/only"));
#else
    // Lowercase "path" is a new variable; the real PATH is untouched.
    CHECK(contains(entries, "path=/only"));
    CHECK(count_with_name(entries, "PATH") == 1);
#endif
}

// --- launching ------------------------------------------------------------------

TEST_CASE("launching a nonexistent executable throws rather than returning") {
    // Both platforms report this synchronously, as an exception, rather than
    // producing a transport that looks alive.
    CHECK_THROWS_AS(ChildProcessTransport({"no_such_program_xyz_12345"}),
                    proxima::KernelError);
    CHECK_THROWS_AS(ChildProcessTransport({}), proxima::KernelError);
}

TEST_CASE("what a real child writes comes back") {
    // The command is written so the token appears in the output only if the
    // command actually ran: cmd.exe drops the caret and the shell the quotes,
    // so an echo of the input itself would not match.
#ifdef _WIN32
    ChildProcessTransport child({command_shell(), "/q"});
    child.send("echo mx_transport^_ok\r\n");
#else
    ChildProcessTransport child({"/bin/sh"});
    child.send("echo mx_transport'_'ok\n");
#endif
    CHECK(read_until(child, "mx_transport_ok").find("mx_transport_ok")
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
        {command_shell(), "/c", "ping -n 30 127.0.0.1 >nul"});
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
        const auto kill_start = std::chrono::steady_clock::now();
        child.kill();
        const auto took = std::chrono::steady_clock::now() - kill_start;
        CHECK(took >= 1s); // The grace period was given.
        CHECK(took < 10s);
        CHECK_FALSE(child.alive());
    }

    SUBCASE("while terminate() ends it without the grace period") {
        // What recovery after a timeout wants: the child was never asked to
        // leave, so there is nothing to wait for.
        const auto terminate_start = std::chrono::steady_clock::now();
        child.terminate();
        CHECK(std::chrono::steady_clock::now() - terminate_start < 1s);
        CHECK_FALSE(child.alive());
    }
}

TEST_CASE("a slow child: many empty reads, then more than one read's worth") {
    // What a Maxima session looks like at startup: nothing for a while, which
    // the session waits out in short reads, then a burst larger than a single
    // read. Accumulated the way MaximaSession::read_frame accumulates, into a
    // string that lives across every call.
    //
    // It cannot catch everything that goes wrong at that point. cmd.exe and sh
    // write without overlapped I/O, so the Windows completion-port corruption
    // described in child_process.cpp never shows here; it only ever showed with
    // SBCL, and the Maxima integration suite is what guards against it.
    //
    // The burst is generated rather than taken from a directory listing, so it
    // is reliably larger than one read now that a read is 64 KB: about 240 KB.
#ifdef _WIN32
    ChildProcessTransport child(
        {command_shell(), "/c",
         "ping -n 2 127.0.0.1 >nul & for /l %i in (1,1,4000) do "
         "@echo mx_filler_line_0123456789_abcdefghijklmnopqrstuvwxyz"});
#else
    ChildProcessTransport child(
        {"/bin/sh", "-c",
         "sleep 1; yes mx_filler_line_0123456789_abcdefghijklmnopqrstuvwxyz"
         " | head -c 240000"});
#endif

    std::string output;
    int empty_reads = 0;
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (std::chrono::steady_clock::now() < deadline) {
        const std::string chunk = child.receive(50ms);
        if (chunk.empty()) {
            if (!child.alive()) {
                break;
            }
            ++empty_reads;
            continue;
        }
        output += chunk;
    }

    CHECK(empty_reads >= 5);
    // More than one read's worth, with the transport's 64 KB reads.
    CHECK(output.size() > 64 * 1024);
}

TEST_CASE("an environment override reaches the child") {
#ifdef _WIN32
    ChildProcessTransport child({command_shell(), "/c", "echo %PROXIMA_TRANSPORT_TEST%"},
                                {{"PROXIMA_TRANSPORT_TEST", "value42"}});
#else
    ChildProcessTransport child({"/bin/sh", "-c", "echo \"$PROXIMA_TRANSPORT_TEST\""},
                                {{"PROXIMA_TRANSPORT_TEST", "value42"}});
#endif
    // Without the override the output would be the unexpanded name, or empty.
    CHECK(read_until(child, "value42").find("value42") != std::string::npos);
}

// --- non-ASCII ------------------------------------------------------------------
//
// A Latin letter and two CJK characters, which no single ANSI code page holds,
// in UTF-8 — the encoding the transport takes on every platform. Written as
// bytes so the test does not depend on the source encoding.

namespace {
constexpr const char *kUnicodeBytes
    = "m" "\xC3\xA6" "xima_" "\xE4\xB8\xAD" "\xE6\x96\x87";
}

TEST_CASE("an executable under a non-ASCII directory starts") {
    // The shell copied into a directory with the name, then launched from
    // there. If the executable path went through the ANSI code page the file
    // would not be found and the constructor would throw.
    const std::filesystem::path dir
        = std::filesystem::temp_directory_path()
          / proxima::detail::path_from_utf8(std::string("mx_transport_") + kUnicodeBytes);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    REQUIRE_FALSE(ec);

#ifdef _WIN32
    const std::filesystem::path shell = dir / "cmd.exe";
    std::filesystem::copy_file(command_shell(), shell,
                               std::filesystem::copy_options::overwrite_existing,
                               ec);
#else
    const std::filesystem::path shell = dir / "sh";
    std::filesystem::copy_file("/bin/sh", shell,
                               std::filesystem::copy_options::overwrite_existing,
                               ec);
    std::filesystem::permissions(shell, std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::add);
#endif
    REQUIRE_FALSE(ec);

    {
#ifdef _WIN32
        ChildProcessTransport child({proxima::detail::to_utf8(shell), "/c",
                                     "echo mx_unicode^_ok"});
#else
        ChildProcessTransport child({proxima::detail::to_utf8(shell), "-c",
                                     "echo mx_unicode'_'ok"});
#endif
        CHECK(read_until(child, "mx_unicode_ok").find("mx_unicode_ok")
              != std::string::npos);
    }

    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("non-ASCII arguments and environment values arrive intact") {
#ifdef _WIN32
    // cmd.exe compares the two in UTF-16, so the answer does not depend on
    // the console code page its output would be written in. Both have to be
    // the same characters — and the variable expanded at all — to match.
    ChildProcessTransport child(
        {command_shell(), "/c",
         std::string("if \"%PROXIMA_TRANSPORT_TEST%\"==\"") + kUnicodeBytes
             + "\" (echo mx_same) else (echo mx_different)"},
        {{"PROXIMA_TRANSPORT_TEST", kUnicodeBytes}});
    const std::string output = read_until(child, "mx_");
    CHECK(output.find("mx_same") != std::string::npos);
#else
    // POSIX passes bytes through untouched, so they can be compared directly.
    ChildProcessTransport child(
        {"/bin/sh", "-c", "printf '[%s][%s]' \"$1\" \"$PROXIMA_TRANSPORT_TEST\"", "sh",
         kUnicodeBytes},
        {{"PROXIMA_TRANSPORT_TEST", kUnicodeBytes}});
    const std::string expected
        = std::string("[") + kUnicodeBytes + "][" + kUnicodeBytes + "]";
    CHECK(read_until(child, expected).find(expected) != std::string::npos);
#endif
}

TEST_CASE("an executable path that is not UTF-8 is a KernelError") {
    // Only Windows transcodes, so only Windows can reject it; on POSIX the
    // bytes are a legitimate, if nonexistent, file name.
    CHECK_THROWS_AS(ChildProcessTransport({"no_such\xFF" "program"}),
                    proxima::KernelError);
}

#ifndef _WIN32
TEST_CASE("arguments reach the child exactly as given") {
    // Spaces, quotes and a newline, one argument each. The shell prints each
    // argument it received in brackets.
    ChildProcessTransport child({"/bin/sh", "-c", "printf '[%s]' \"$@\"", "sh",
                                 "a b", "q\"x", "line\nbreak"});
    CHECK(read_until(child, "[line\nbreak]").find("[a b][q\"x][line\nbreak]")
          != std::string::npos);
}
#endif
