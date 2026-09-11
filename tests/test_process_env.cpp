// Environment merging and, on Windows, argv quoting. No child process is
// started here.

#include <doctest/doctest.h>

#include "transport/child_process.hpp"
#include "transport/process_env.hpp"

#include <mx/errors.hpp>

#include <algorithm>
#include <string>
#include <vector>

#ifdef _WIN32
#include "transport/win32_process_utils.hpp"
#endif

using mx::detail::mergeEnvironment;

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

} // namespace

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

#ifdef _WIN32

TEST_CASE("Win32 argument quoting follows the MSVCRT argv rules") {
    using mx::detail::quoteArg;

    // Left alone when there is nothing to escape.
    CHECK(quoteArg("--noinform") == "--noinform");
    CHECK(quoteArg("C:\\maxima\\bin\\sbcl.exe") == "C:\\maxima\\bin\\sbcl.exe");

    // Quoted when it contains whitespace.
    CHECK(quoteArg("C:\\Program Files\\sbcl.exe")
          == "\"C:\\Program Files\\sbcl.exe\"");

    // Embedded quotes are escaped. This is the case that matters: the --eval
    // argument is a Lisp form full of them.
    CHECK(quoteArg("(setf x \"y\")") == "\"(setf x \\\"y\\\")\"");

    // Backslashes are only doubled when they precede a quote.
    CHECK(quoteArg("a\\b c") == "\"a\\b c\"");
    CHECK(quoteArg("a\\\"b") == "\"a\\\\\\\"b\"");
}

TEST_CASE("a command line joins quoted arguments with spaces") {
    using mx::detail::buildCommandLine;
    CHECK(buildCommandLine({"sbcl.exe", "--core", "C:\\a b\\x.core"})
          == "sbcl.exe --core \"C:\\a b\\x.core\"");
}

TEST_CASE("environment block is double-NUL terminated") {
    using mx::detail::buildEnvironmentBlock;
    const auto block = buildEnvironmentBlock({{"MX_TEST_X", "1"}});
    REQUIRE(block.size() >= 2);
    CHECK(block.back() == '\0');
    CHECK(block[block.size() - 2] == '\0');
}

#endif // _WIN32

TEST_CASE("launching a nonexistent executable throws rather than returning") {
    // On Windows CreateProcess reports this synchronously; on POSIX the failure
    // comes back through the close-on-exec status pipe. Both must surface it as
    // an exception rather than a transport that looks alive.
    using mx::detail::ChildProcessTransport;
    CHECK_THROWS_AS(ChildProcessTransport({"no_such_program_xyz_12345"}),
                    mx::KernelError);
    CHECK_THROWS_AS(ChildProcessTransport({}), mx::KernelError);
}
