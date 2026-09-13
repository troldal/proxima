// Numeric evaluation. Almost all of this needs no kernel: once Maxima has
// produced a closed form, turning it into numbers is ordinary arithmetic.

#include <doctest/doctest.h>

#include <mx/errors.hpp>
#include <mx/expr.hpp>
#include <mx/functions.hpp>
#include <mx/numeric.hpp>
#include <mx/ops.hpp>
#include <mx/symbol.hpp>
#include <mx/version.hpp>

#include <atomic>
#include <cmath>
#include <limits>
#include <numbers>
#include <span>
#include <string>
#include <thread>
#include <vector>

using mx::Expr;
using mx::Symbol;

namespace {

/// doctest::Approx cannot compare infinities — its relative-epsilon arithmetic
/// gives NaN — and `x^-3` at zero is a perfectly ordinary infinity here.
bool sameNumber(double a, double b) {
    if (std::isnan(a) || std::isnan(b)) {
        return std::isnan(a) && std::isnan(b);
    }
    if (std::isinf(a) || std::isinf(b)) {
        return a == b;
    }
    return a == doctest::Approx(b);
}

} // namespace

TEST_CASE("numeric leaves evaluate to themselves") {
    CHECK(mx::evalNumeric(Expr(42)) == doctest::Approx(42.0));
    CHECK(mx::evalNumeric(Expr(-7)) == doctest::Approx(-7.0));
    CHECK(mx::evalNumeric(Expr(1.5)) == doctest::Approx(1.5));
    CHECK(mx::evalNumeric(Expr::rational(1, 4)) == doctest::Approx(0.25));
}

TEST_CASE("symbols take their value from the bindings") {
    const Symbol x("x");
    const Symbol y("y");

    CHECK(mx::evalNumeric(Expr(x), {{"x", 3.0}}) == doctest::Approx(3.0));
    CHECK(mx::evalNumeric(Expr(x) * Expr(y), {{"x", 3.0}, {"y", 4.0}})
          == doctest::Approx(12.0));
}

TEST_CASE("arithmetic") {
    const Symbol x("x");
    const mx::Bindings at2{{"x", 2.0}};

    CHECK(mx::evalNumeric(pow(Expr(x), 2) + 3 * Expr(x) + 2, at2)
          == doctest::Approx(12.0));
    CHECK(mx::evalNumeric(Expr(1) / Expr(x), at2) == doctest::Approx(0.5));
    CHECK(mx::evalNumeric(-Expr(x), at2) == doctest::Approx(-2.0));
    CHECK(mx::evalNumeric(mx::sqrt(Expr(x)), at2)
          == doctest::Approx(std::sqrt(2.0)));
}

TEST_CASE("the functions Maxima leaves in a result") {
    const Symbol x("x");
    const mx::Bindings at1{{"x", 1.0}};

    CHECK(mx::evalNumeric(mx::sin(Expr(x)), at1) == doctest::Approx(std::sin(1.0)));
    CHECK(mx::evalNumeric(mx::cos(Expr(x)), at1) == doctest::Approx(std::cos(1.0)));
    CHECK(mx::evalNumeric(mx::log(Expr(x)), at1) == doctest::Approx(0.0));
    CHECK(mx::evalNumeric(mx::abs(-Expr(x)), at1) == doctest::Approx(1.0));
    CHECK(mx::evalNumeric(mx::exp(Expr(x)), at1) == doctest::Approx(std::numbers::e));
    CHECK(mx::evalNumeric(Expr::function("max", {Expr(1), Expr(5), Expr(3)}))
          == doctest::Approx(5.0));
}

TEST_CASE("constants are recognised as Maxima spells them") {
    CHECK(mx::evalNumeric(mx::pi()) == doctest::Approx(std::numbers::pi));
    CHECK(mx::evalNumeric(mx::e()) == doctest::Approx(std::numbers::e));
    CHECK(std::isinf(mx::evalNumeric(mx::inf())));
    CHECK(mx::evalNumeric(mx::minusInf()) < 0);

    SUBCASE("but an explicit binding still wins") {
        // So a symbol that happens to be named %e can be given a value.
        CHECK(mx::evalNumeric(mx::e(), {{"%e", 10.0}}) == doctest::Approx(10.0));
    }
}

TEST_CASE("what cannot be evaluated says so rather than guessing") {
    const Symbol x("x");

    SUBCASE("an unbound symbol") {
        CHECK_THROWS_AS(mx::evalNumeric(Expr(x)), mx::EvalError);
    }
    SUBCASE("a function with no numeric meaning here") {
        // Refusing beats returning something plausible for a function this does
        // not actually implement.
        CHECK_THROWS_AS(
            mx::evalNumeric(Expr::function("bessel_j", {Expr(0), Expr(1)})),
            mx::EvalError);
    }
    SUBCASE("a relation") {
        CHECK_THROWS_AS(mx::evalNumeric(eq(Expr(1), Expr(1))), mx::EvalError);
    }
    SUBCASE("an Opaque node") {
        // Maxima source this library never interpreted, so there is nothing
        // here that could evaluate it.
        CHECK_THROWS_AS(mx::evalNumeric(Expr::opaque("30!")), mx::EvalError);
    }
    SUBCASE("and the message names the culprit") {
        try {
            mx::evalNumeric(Expr(x) + 1);
            FAIL("expected an EvalError");
        } catch (const mx::EvalError &e) {
            CHECK(std::string(e.what()).find("x") != std::string::npos);
        }
    }
}

TEST_CASE("isEvaluable answers without throwing") {
    const Symbol x("x");
    CHECK_FALSE(mx::isEvaluable(Expr(x)));
    CHECK(mx::isEvaluable(Expr(x), {{"x", 1.0}}));
    CHECK(mx::isEvaluable(mx::pi()));
    CHECK_FALSE(mx::isEvaluable(Expr::opaque("30!")));
}

TEST_CASE("asFunction binds one variable for repeated use") {
    const Symbol x("x");
    const auto f = mx::asFunction(pow(Expr(x), 2), x);

    CHECK(f(0.0) == doctest::Approx(0.0));
    CHECK(f(3.0) == doctest::Approx(9.0));

    SUBCASE("with the other symbols fixed") {
        const Symbol a("a");
        const auto scaled = mx::asFunction(Expr(a) * Expr(x), x, {{"a", 10.0}});
        CHECK(scaled(2.5) == doctest::Approx(25.0));
    }
}

TEST_CASE("the version header reports the project version") {
    CHECK(std::string(mx::version) == "0.1.0");
    CHECK(mx::versionMajor == 0);
}

// --- the compiled form ----------------------------------------------------

TEST_CASE("a compiled expression agrees with the one-shot evaluator") {
    // Two implementations, so the risk is that they drift. They share the
    // function table; this checks the rest.
    const Symbol x("x");
    const Symbol y("y");

    for (const char *source : {
             "x + 1",
             "x^2 - 3*x + 2",
             "sin(x)*cos(x) + log(1 + x^2)",
             "x/(1 + x^2)",
             "sqrt(abs(x)) + %pi*x",
             "max(x, 1, 2) + min(x, 0)",
             "atan2(x, 2) + mod(x, 3)",
             "(1 + x)^7",
             "x^(-3)",
             "2^x",
         }) {
        const std::string text = source;
        CAPTURE(text);
        const Expr f = Expr::parse(source);
        const mx::Compiled compiled(f, x);

        for (const double at : {-2.5, -1.0, -0.25, 0.0, 0.5, 1.0, 3.75}) {
            CAPTURE(at);
            CHECK(sameNumber(compiled(at), mx::evalNumeric(f, {{"x", at}})));
        }
    }
}

TEST_CASE("integer powers are specialised without changing the answer") {
    // std::pow is replaced by squaring where the exponent is a small integer.
    // The results must be indistinguishable, including at the awkward values.
    const Symbol x("x");

    for (int exponent = -8; exponent <= 8; ++exponent) {
        CAPTURE(exponent);
        const Expr f = pow(Expr(x), Expr(exponent));
        const mx::Compiled compiled(f, x);

        for (const double base : {-3.0, -1.0, -0.5, 0.5, 1.0, 2.0}) {
            CAPTURE(base);
            CHECK(sameNumber(compiled(base), std::pow(base, exponent)));
        }
    }

    SUBCASE("including the degenerate cases") {
        CHECK(mx::Compiled(pow(Expr(x), 0), x)(0.0)
              == doctest::Approx(std::pow(0.0, 0)));
        CHECK(std::isinf(mx::Compiled(pow(Expr(x), -1), x)(0.0)));
    }

    SUBCASE("and an exponent too large for the specialisation still works") {
        const Expr big = pow(Expr(x), 200);
        CHECK(mx::Compiled(big, x)(1.05)
              == doctest::Approx(std::pow(1.05, 200)));
    }
}

TEST_CASE("several variables, in the order given") {
    const Symbol x("x");
    const Symbol y("y");
    const std::vector<Symbol> variables{x, y};

    const mx::Compiled compiled(Expr::parse("x^2 + 2*y"), variables);
    REQUIRE(compiled.arity() == 2);
    CHECK(compiled.variableNames() == std::vector<std::string>{"x", "y"});

    const double point[] = {3.0, 5.0};
    CHECK(compiled(point) == doctest::Approx(19.0));

    SUBCASE("and the order is the caller's, not the expression's") {
        const std::vector<Symbol> reversed{y, x};
        const mx::Compiled swapped(Expr::parse("x^2 + 2*y"), reversed);
        const double sameValues[] = {5.0, 3.0}; // y, x
        CHECK(swapped(sameValues) == doctest::Approx(19.0));
    }
}

TEST_CASE("what shadows what") {
    const Symbol x("x");
    const Symbol e("%e");

    // A variable beats a binding, which beats a named constant.
    CHECK(mx::Compiled(Expr::symbol("%e"), e)(7.0) == doctest::Approx(7.0));
    CHECK(mx::Compiled(Expr::symbol("%e"), x, {{"%e", 5.0}})(0.0)
          == doctest::Approx(5.0));
    CHECK(mx::Compiled(Expr::symbol("%e"), x)(0.0)
          == doctest::Approx(std::numbers::e));
}

TEST_CASE("errors surface at construction, not on every call") {
    // An unknown function or an unbound symbol is a property of the expression,
    // not of the point being evaluated, so it should be reported once.
    const Symbol x("x");

    CHECK_THROWS_AS(mx::Compiled(Expr::symbol("y"), x), mx::EvalError);
    CHECK_THROWS_AS(
        mx::Compiled(Expr::function("bessel_j", {Expr(0), Expr(x)}), x),
        mx::EvalError);
    CHECK_THROWS_AS(mx::Compiled(eq(Expr(x), Expr(1)), x), mx::EvalError);
    CHECK_THROWS_AS(mx::Compiled(Expr::opaque("30!"), x), mx::EvalError);

    SUBCASE("and the wrong number of values is refused") {
        const mx::Compiled compiled(Expr(x), x);
        const double none[] = {0.0};
        CHECK_THROWS_AS(compiled(std::span<const double>(none, 0)),
                        mx::EvalError);
    }
}

TEST_CASE("repeated literals share one constant slot") {
    // Not observable in the answer, only in the size — but it is the sort of
    // thing that silently stops working, so it is worth pinning.
    const Symbol x("x");
    const mx::Compiled compiled(Expr::parse("2*x + 2*x^2 + 2"), x);
    CHECK(compiled.size() > 0);
    CHECK(compiled(1.0) == doctest::Approx(6.0));
}

TEST_CASE("one compiled expression can be shared between threads") {
    // The working stack is thread-local, so no synchronisation is needed.
    const Symbol x("x");
    const mx::Compiled compiled(Expr::parse("sin(x)^2 + cos(x)^2"), x);

    std::vector<std::thread> threads;
    std::atomic<int> wrong{0};
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&compiled, &wrong, t] {
            for (int i = 0; i < 2000; ++i) {
                const double at = t + i * 1e-3;
                if (std::fabs(compiled(at) - 1.0) > 1e-9) {
                    ++wrong;
                }
            }
        });
    }
    for (std::thread &thread : threads) {
        thread.join();
    }
    CHECK(wrong == 0);
}

TEST_SUITE("maxima") {

TEST_CASE("a result from Maxima can be evaluated without another round trip") {
    // The whole point: one call to find the closed form, then arithmetic.
    const Symbol x("x");

    const auto antiderivative
        = mx::integrate(pow(Expr(x), 2) * mx::sin(Expr(x)), x);
    REQUIRE(antiderivative.has_value());

    // 2x sin(x) + (2 - x^2) cos(x), evaluated at 1.
    const double expected = 2 * std::sin(1.0) + (2 - 1) * std::cos(1.0);
    CHECK(mx::evalNumeric(*antiderivative, {{"x", 1.0}})
          == doctest::Approx(expected));
}

TEST_CASE("a definite integral agrees with sampling its antiderivative") {
    // An end-to-end check that the symbolic and numeric halves agree:
    // F(1) - F(0) should equal what Maxima says the definite integral is.
    const Symbol x("x");
    const Expr integrand = pow(Expr(x), 3) + Expr(x);

    const auto exact = mx::integrate(integrand, x, Expr(0), Expr(1));
    REQUIRE(exact.has_value());

    const auto antiderivative = mx::integrate(integrand, x);
    REQUIRE(antiderivative.has_value());
    const auto f = mx::asFunction(*antiderivative, x);

    CHECK(f(1.0) - f(0.0) == doctest::Approx(mx::evalNumeric(*exact)));
}

} // TEST_SUITE("maxima")

// --- functions whose C library namesakes mean something else ----------------

TEST_CASE("mod and round are Maxima's, not the C library's") {
    // Both used to be the <cmath> function of the same name, which answers a
    // different question: std::fmod truncates towards zero and std::round
    // rounds halves away from zero. Every expectation below is the answer
    // Maxima itself gave.
    const auto mod = [](double a, double b) {
        return mx::evalNumeric(Expr::function("mod", {Expr(a), Expr(b)}));
    };
    const auto round = [](double value) {
        return mx::evalNumeric(Expr::function("round", {Expr(value)}));
    };

    SUBCASE("mod takes the sign of the divisor") {
        CHECK(mod(7, 3) == 1.0);
        CHECK(mod(-7, 3) == 2.0); // Was -1.
        CHECK(mod(7, -3) == -2.0);
        CHECK(mod(-7, -3) == -1.0);
        CHECK(mod(7.5, 2) == 1.5);
        CHECK(mod(-7.5, 2) == 0.5);
        CHECK(mod(7.5, -2) == -0.5);
        CHECK(mod(5, 2.5) == 0.0);
        CHECK(mod(0, 3) == 0.0);
    }
    SUBCASE("a remainder of zero is not a negative zero") {
        CHECK(mod(-6, 3) == 0.0);
        CHECK_FALSE(std::signbit(mod(-6, 3)));
    }
    SUBCASE("mod by zero is the dividend") {
        // Was NaN.
        CHECK(mod(7, 0) == 7.0);
        CHECK(mod(-7.5, 0) == -7.5);
    }
    SUBCASE("mod is exact where Maxima's float arithmetic is not") {
        // A deliberate difference. 1e20 is exactly representable and leaves a
        // remainder of 1; Maxima computes x - y*floor(x/y) in floating point,
        // which rounds, and answers 0.0.
        CHECK(mod(1e20, 3) == 1.0);
    }

    SUBCASE("round sends halves to the even neighbour") {
        CHECK(round(2.5) == 2.0); // Was 3.
        CHECK(round(3.5) == 4.0);
        CHECK(round(1.5) == 2.0);
        CHECK(round(-2.5) == -2.0); // Was -3.
        CHECK(round(-3.5) == -4.0);
        CHECK(round(0.5) == 0.0);
    }
    SUBCASE("and anything else to the nearest") {
        CHECK(round(2.4999) == 2.0);
        CHECK(round(2.6) == 3.0);
        CHECK(round(-2.6) == -3.0);
        CHECK(round(7) == 7.0);
        CHECK(round(4503599627370497.0) == 4503599627370497.0);
    }
    SUBCASE("rounding -0.5 gives a zero with no sign") {
        CHECK(round(-0.5) == 0.0);
        CHECK_FALSE(std::signbit(round(-0.5)));
    }
    SUBCASE("infinities and NaN pass through") {
        CHECK(std::isinf(round(std::numeric_limits<double>::infinity())));
        CHECK(round(-std::numeric_limits<double>::infinity()) < 0);
        CHECK(std::isnan(round(std::nan(""))));
    }

    SUBCASE("the compiled form agrees, since it shares the function table") {
        const Symbol x("x");
        const mx::Compiled compiledMod(Expr::function("mod", {Expr(x), Expr(3)}),
                                       x);
        CHECK(compiledMod(-7.0) == 2.0);
        CHECK(compiledMod(-6.0) == 0.0);
        const mx::Compiled compiledRound(Expr::function("round", {Expr(x)}), x);
        CHECK(compiledRound(2.5) == 2.0);
        CHECK(compiledRound(-2.5) == -2.0);
    }
}

TEST_SUITE("maxima") {

TEST_CASE("mod and round agree with Maxima across signs and halves") {
    // The builtins are the one place this library computes something Maxima
    // also computes, so they are checked against it directly — the way
    // test_integer.cpp checks bignum arithmetic — rather than against
    // expectations written by whoever wrote the code.
    mx::Kernel kernel;

    // Integral values are sent as integers, which is how they would appear in
    // a closed form, and which sidesteps asking what mod(x, 0.0) means.
    const auto text = [](double value) {
        if (std::floor(value) == value && std::fabs(value) < 1e15) {
            return Expr(static_cast<long long>(value)).str();
        }
        return Expr(value).str();
    };
    const auto maxima = [&kernel](const std::string &source) {
        const auto answer = mx::parse(source, kernel);
        REQUIRE_MESSAGE(answer.has_value(), source);
        return mx::evalNumeric(*answer);
    };

    const double dividends[] = {-7.5, -7, -6, -2.5, -0.5, 0, 0.5, 2.5, 6, 7, 7.5};
    const double divisors[] = {-3, -2, 2, 2.5, 3, 0};
    for (const double a : dividends) {
        for (const double b : divisors) {
            const std::string source = "mod(" + text(a) + ", " + text(b) + ")";
            CAPTURE(source);
            const double local
                = mx::evalNumeric(Expr::function("mod", {Expr(a), Expr(b)}));
            CHECK(sameNumber(local, maxima(source)));
        }
    }

    const double values[] = {-3.5, -2.6, -2.5, -1.5, -0.5, 0.5,
                             1.5,  2.4999, 2.5, 2.6, 3.5, 7};
    for (const double value : values) {
        const std::string source = "round(" + text(value) + ")";
        CAPTURE(source);
        const double local
            = mx::evalNumeric(Expr::function("round", {Expr(value)}));
        CHECK(sameNumber(local, maxima(source)));
    }
}

} // TEST_SUITE("maxima")
