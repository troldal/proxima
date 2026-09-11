// Integration tests: these start a real Maxima process. Excluded from the
// default unit run via `ctest -LE maxima`.
//
// From PLAN.md step 4 these stop being the only way to exercise the kernel,
// because FakeTransport lets the protocol be tested without an installation.

#include <doctest/doctest.h>

#include <mx/config.hpp>
#include <mx/errors.hpp>
#include <mx/kernel.hpp>

#include <filesystem>
#include <string>

TEST_SUITE("maxima") {

TEST_CASE("kernel evaluates arithmetic") {
    mx::Kernel maxima;
    CHECK(maxima.evalRaw("1 + 1;") == "2");
}

TEST_CASE("kernel returns nothing for a $-terminated statement") {
    mx::Kernel maxima;
    CHECK(maxima.evalRaw("42$") == "");
}

TEST_CASE("session state persists across calls") {
    // The whole point of holding the process open: a binding made by one call
    // is still there for the next. A per-call process would fail this.
    mx::Kernel maxima;
    REQUIRE(maxima.evalRaw("a: 7$") == "");
    CHECK(maxima.evalRaw("a^2;") == "49");
}

TEST_CASE("kernel performs a symbolic integration") {
    mx::Kernel maxima;
    const std::string result = maxima.evalRaw("integrate(x^2*sin(x), x);");
    CHECK_FALSE(result.empty());
    CHECK(result.find("sin") != std::string::npos);
}

TEST_CASE("the launch environment reaches the child process") {
    // Maxima exposes the value of $MAXIMA_USERDIR as maxima_userdir, which
    // makes the whole environment-block path observable end to end: merged over
    // the inherited environment, passed to CreateProcess, and read by Maxima.
    //
    // This is also what keeps the user's own maxima-init.mac out of the
    // picture, so it is worth asserting rather than assuming.
    mx::Config config;
    config.userDir = std::filesystem::temp_directory_path() / "mx_test_userdir";

    // Maxima reports the directory with forward slashes, which is how the
    // launch environment exports it on both platforms.
    const std::string expected = "\"" + config.userDir.generic_string() + "\"";

    mx::Kernel maxima(config);
    CHECK(maxima.evalRaw("maxima_userdir;") == expected);
}

TEST_CASE("an explicitly wrong Maxima root fails loudly") {
    mx::Config config;
    config.maximaRoot
        = std::filesystem::temp_directory_path() / "no_such_maxima_install";
    CHECK_THROWS_AS(mx::Kernel{config}, mx::KernelError);
}

TEST_CASE("two kernels are independent") {
    mx::Kernel first;
    mx::Kernel second;
    REQUIRE(first.evalRaw("b: 1$") == "");
    // `second` never saw the binding, so Maxima echoes the symbol back.
    CHECK(second.evalRaw("b;") == "b");
}

} // TEST_SUITE("maxima")
