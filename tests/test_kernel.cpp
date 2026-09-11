// Integration tests: these start a real Maxima process. Excluded from the
// default unit run via `ctest -LE maxima`.

#include <doctest/doctest.h>

#include <mx/config.hpp>
#include <mx/errors.hpp>
#include <mx/kernel.hpp>

#include <filesystem>
#include <string>

TEST_SUITE("maxima") {

TEST_CASE("kernel evaluates arithmetic") {
    mx::Kernel maxima;
    const mx::Reply reply = maxima.eval("1 + 1");
    REQUIRE(reply.ok);
    CHECK(reply.value == "2");
}

TEST_CASE("results arrive as Maxima's internal s-expression") {
    mx::Kernel maxima;
    // Not display output: a tagged tree with no precedence to re-derive.
    CHECK(maxima.eval("x + 1").value == "((MPLUS SIMP) 1 $X)");
    CHECK(maxima.eval("sin(x)").value == "((%SIN SIMP) $X)");
}

TEST_CASE("exact rationals are preserved rather than rounded") {
    // The single strongest reason for using the internal form. Display output
    // and every double-based expression library would give 0.7333...
    mx::Kernel maxima;
    CHECK(maxima.eval("1/3 + 2/5").value == "((RAT SIMP) 11 15)");
}

TEST_CASE("a Maxima error is reported, not thrown") {
    mx::Kernel maxima;
    const mx::Reply reply = maxima.eval("integrate(x, 5)");
    CHECK_FALSE(reply.ok);
    CHECK(reply.reason.find("must not be a number") != std::string::npos);
}

TEST_CASE("the session keeps working after an error") {
    // errcatch exists precisely so a failure does not leave the stream in an
    // error prompt, off by one for every subsequent reply.
    mx::Kernel maxima;
    REQUIRE_FALSE(maxima.eval("integrate(x, 5)").ok);
    const mx::Reply after = maxima.eval("2 + 2");
    CHECK(after.ok);
    CHECK(after.value == "4");
}

TEST_CASE("session state persists across calls") {
    // The whole point of holding the process open: a binding made by one call
    // is still there for the next.
    mx::Kernel maxima;
    REQUIRE(maxima.eval("a: 7").ok);
    CHECK(maxima.eval("a^2").value == "49");
}

TEST_CASE("kernel performs a symbolic integration") {
    mx::Kernel maxima;
    const mx::Reply reply = maxima.eval("integrate(x^2*sin(x), x)");
    REQUIRE(reply.ok);
    CHECK(reply.value.find("%SIN") != std::string::npos);
    CHECK(reply.value.find("%COS") != std::string::npos);
}

TEST_CASE("many requests in a row stay correlated") {
    // Each reply must match the request that asked for it. An off-by-one here
    // would still look entirely plausible, which is why the ids exist.
    mx::Kernel maxima;
    for (int i = 1; i <= 20; ++i) {
        const mx::Reply reply = maxima.eval(std::to_string(i) + " * 10");
        REQUIRE(reply.ok);
        CHECK(reply.value == std::to_string(i * 10));
    }
}

TEST_CASE("an unknown function comes back as an uninterpreted application") {
    // The escape hatch that keeps the term layer's node set small: anything
    // Maxima knows and we do not still round-trips.
    mx::Kernel maxima;
    CHECK(maxima.eval("f(x)").value == "(($F SIMP) $X)");
}

TEST_CASE("the launch environment reaches the child process") {
    // Maxima exposes the value of $MAXIMA_USERDIR as maxima_userdir, which
    // makes the whole environment-block path observable end to end. It is also
    // what keeps the user's own maxima-init.mac out of the picture.
    mx::Config config;
    config.userDir = std::filesystem::temp_directory_path() / "mx_test_userdir";
    const std::string expected = "\"" + config.userDir.generic_string() + "\"";

    mx::Kernel maxima(config);
    CHECK(maxima.eval("maxima_userdir").value == expected);
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
    REQUIRE(first.eval("b: 1").ok);
    // `second` never saw the binding, so Maxima echoes the symbol back.
    CHECK(second.eval("b").value == "$B");
}

} // TEST_SUITE("maxima")
