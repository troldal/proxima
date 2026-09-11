// Integration tests: these start a real Maxima process. Excluded from the
// default unit run via `ctest -LE maxima`.
//
// From PLAN.md step 4 these stop being the only way to exercise the kernel,
// because FakeTransport lets the protocol be tested without an installation.

#include <doctest/doctest.h>

#include <mx/kernel.hpp>

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

TEST_CASE("two kernels are independent") {
    mx::Kernel first;
    mx::Kernel second;
    REQUIRE(first.evalRaw("b: 1$") == "");
    // `second` never saw the binding, so Maxima echoes the symbol back.
    CHECK(second.evalRaw("b;") == "b");
}

} // TEST_SUITE("maxima")
