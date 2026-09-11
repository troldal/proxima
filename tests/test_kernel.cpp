// Integration tests: these start a real Maxima process. Excluded from the
// default unit run via `ctest -LE maxima`.

#include <doctest/doctest.h>

#include <mx/config.hpp>
#include <mx/errors.hpp>
#include <mx/expr.hpp>
#include <mx/kernel.hpp>
#include <mx/symbol.hpp>

#include "wire/sexpr.hpp"

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

TEST_CASE("live replies parse as s-expressions") {
    // The golden file checks the reader against recorded output; this checks it
    // against output produced right now, which is what catches the recording
    // going stale after a Maxima upgrade.
    mx::Kernel maxima;
    for (const char *expression : {
             "1/3 + 2/5",
             "integrate(x^2*sin(x), x)",
             "solve(x^2 - 1 = 0, x)",
             "expand((x + 1)^3)",
             "matrix([1,2],[3,4])",
             "30!",
             "bfloat(%pi)",
             "f(x, y)",
             "[a, b, c]",
         }) {
        CAPTURE(expression);
        const mx::Reply reply = maxima.eval(expression);
        REQUIRE(reply.ok);

        mx::detail::SExpr parsed;
        REQUIRE_NOTHROW(parsed = mx::detail::parseSExpr(reply.value));
        // Re-rendering must read back identically, which catches a reader that
        // accepts input but quietly loses part of it.
        CHECK(mx::detail::parseSExpr(parsed.toString()) == parsed);
    }
}

TEST_CASE("a live rational keeps its exact numerator and denominator") {
    mx::Kernel maxima;
    const mx::detail::SExpr rational
        = mx::detail::parseSExpr(maxima.eval("1/3 + 2/5").value);

    REQUIRE(rational.isList());
    CHECK(rational.at(0).at(0).isSymbol("RAT"));
    CHECK(rational.at(1).asInt64() == 11);
    CHECK(rational.at(2).asInt64() == 15);
}

TEST_CASE("a live bignum survives as digits") {
    // 30! does not fit in int64, so the reader must keep it as text rather than
    // wrap or truncate.
    mx::Kernel maxima;
    const mx::detail::SExpr value
        = mx::detail::parseSExpr(maxima.eval("30!").value);

    REQUIRE(value.isInteger());
    CHECK(value.digits() == "265252859812191058636308480000000");
    CHECK_FALSE(value.asInt64().has_value());
}

TEST_CASE("printed expressions are valid Maxima meaning the same thing") {
    // Expr::str() claims to emit Maxima-compatible infix. That claim is only
    // worth anything if Maxima agrees, so each case below is one where dropping
    // the parentheses would still *parse* but quietly mean something else.
    mx::Kernel maxima;
    const mx::Symbol x("x");
    const mx::Symbol y("y");

    const auto agreesWith = [&maxima](const mx::Expr &expr,
                                      const std::string &reference) {
        const mx::Reply printed = maxima.eval("ratsimp(" + expr.str() + ")");
        const mx::Reply expected = maxima.eval("ratsimp(" + reference + ")");
        REQUIRE_MESSAGE(printed.ok, expr.str() << " -> " << printed.reason);
        REQUIRE(expected.ok);
        return printed.value == expected.value;
    };

    // -3^2 is -9 in Maxima; the base needs its own parentheses.
    CHECK(agreesWith(pow(mx::Expr(-3), 2), "9"));
    // 3/2^2 is 3/4; (3/2)^2 is 9/4.
    CHECK(agreesWith(pow(mx::Expr::rational(3, 2), 2), "9/4"));
    // ^ is right-associative, so x^2^3 is x^8.
    CHECK(agreesWith(pow(pow(mx::Expr(x), 2), 3), "x^6"));
    // x+1*y is x+y.
    CHECK(agreesWith((mx::Expr(x) + 1) * mx::Expr(y), "(x+1)*y"));
    // x*-2 is not valid Maxima at all, so this one tests that it parses.
    CHECK(agreesWith(mx::Expr::mul({mx::Expr(x), mx::Expr(-2)}), "-2*x"));
    // A leading negative factor needs no parentheses, and must not gain any
    // that change its meaning.
    CHECK(agreesWith(-mx::Expr(x) + 1, "1-x"));
    CHECK(agreesWith(mx::Expr(x) - 3 * mx::Expr(x), "-2*x"));
    // Exact division stays exact rather than becoming a float.
    CHECK(agreesWith(mx::Expr(1) / mx::Expr(3), "1/3"));
    // Relations and uninterpreted applications survive the trip.
    CHECK(agreesWith(mx::Expr::function("bessel_j", {mx::Expr(0), mx::Expr(x)}),
                     "bessel_j(0, x)"));
}

TEST_CASE("an oversized integer round-trips through Opaque") {
    // 30! does not fit in mx::Integer, so it is carried as Opaque text. It must
    // still print as something Maxima reads back as the same number.
    mx::Kernel maxima;
    const mx::Expr bignum
        = mx::Expr::opaque("265252859812191058636308480000000");

    const mx::Reply reply = maxima.eval("is(" + bignum.str() + " = 30!)");
    REQUIRE(reply.ok);
    CHECK(reply.value == "T");
}

TEST_CASE("two kernels are independent") {
    mx::Kernel first;
    mx::Kernel second;
    REQUIRE(first.eval("b: 1").ok);
    // `second` never saw the binding, so Maxima echoes the symbol back.
    CHECK(second.eval("b").value == "$B");
}

} // TEST_SUITE("maxima")
