// Numeric evaluation. Almost all of this needs no kernel: once Maxima has
// produced a closed form, turning it into numbers is ordinary arithmetic.

#include <doctest/doctest.h>

#include <proxima/errors.hpp>
#include <proxima/expr.hpp>
#include <proxima/functions.hpp>
#include <proxima/numeric.hpp>
#include <proxima/ops.hpp>
#include <proxima/symbol.hpp>
#include <proxima/version.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <concepts>
#include <functional>
#include <limits>
#include <numbers>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using proxima::Expr;
using proxima::Symbol;

namespace {

/// doctest::Approx cannot compare infinities — its relative-epsilon arithmetic
/// gives NaN — and `x^-3` at zero is a perfectly ordinary infinity here.
bool same_number(double a, double b) {
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
    CHECK(*proxima::eval_numeric(Expr(42)) == doctest::Approx(42.0));
    CHECK(*proxima::eval_numeric(Expr(-7)) == doctest::Approx(-7.0));
    CHECK(*proxima::eval_numeric(Expr(1.5)) == doctest::Approx(1.5));
    CHECK(*proxima::eval_numeric(Expr::rational(1, 4)) == doctest::Approx(0.25));
}

TEST_CASE("symbols take their value from the bindings") {
    const Symbol x("x");
    const Symbol y("y");

    CHECK(*proxima::eval_numeric(Expr(x), {{"x", 3.0}}) == doctest::Approx(3.0));
    CHECK(*proxima::eval_numeric(Expr(x) * Expr(y), {{"x", 3.0}, {"y", 4.0}})
          == doctest::Approx(12.0));
}

TEST_CASE("arithmetic") {
    const Symbol x("x");
    const proxima::Bindings at2{{"x", 2.0}};

    CHECK(*proxima::eval_numeric(pow(Expr(x), 2) + 3 * Expr(x) + 2, at2)
          == doctest::Approx(12.0));
    CHECK(*proxima::eval_numeric(Expr(1) / Expr(x), at2) == doctest::Approx(0.5));
    CHECK(*proxima::eval_numeric(-Expr(x), at2) == doctest::Approx(-2.0));
    CHECK(*proxima::eval_numeric(proxima::sqrt(Expr(x)), at2)
          == doctest::Approx(std::sqrt(2.0)));
}

TEST_CASE("the functions Maxima leaves in a result") {
    const Symbol x("x");
    const proxima::Bindings at1{{"x", 1.0}};

    CHECK(*proxima::eval_numeric(proxima::sin(Expr(x)), at1)
          == doctest::Approx(std::sin(1.0)));
    CHECK(*proxima::eval_numeric(proxima::cos(Expr(x)), at1)
          == doctest::Approx(std::cos(1.0)));
    CHECK(*proxima::eval_numeric(proxima::log(Expr(x)), at1)
          == doctest::Approx(0.0));
    CHECK(*proxima::eval_numeric(proxima::abs(-Expr(x)), at1)
          == doctest::Approx(1.0));
    CHECK(*proxima::eval_numeric(proxima::exp(Expr(x)), at1)
          == doctest::Approx(std::numbers::e));
    CHECK(*proxima::eval_numeric(Expr::function("max", {Expr(1), Expr(5), Expr(3)}))
          == doctest::Approx(5.0));
}

TEST_CASE("a long call and a refused expression evaluate as before") {
    // The walk evaluates a call's arguments on the stack, spilling to the heap
    // only past eight, and builds a failure message only when one is wanted.
    const Symbol x("x");

    std::vector<Expr> many;
    for (int i = 1; i <= 12; ++i) {
        many.push_back(Expr(i) * Expr(x));
    }
    CHECK(*proxima::eval_numeric(Expr::function("max", many), {{"x", 2.0}}) == 24.0);
    CHECK(*proxima::eval_numeric(Expr::function("min", many), {{"x", 2.0}}) == 2.0);
    CHECK(proxima::is_evaluable(Expr::function("max", many), {{"x", 2.0}}));

    CHECK_FALSE(proxima::is_evaluable(Expr(x) + 1));
    CHECK_FALSE(proxima::is_evaluable(eq(Expr(x), Expr(1)), {{"x", 1.0}}));
    CHECK_FALSE(proxima::is_evaluable(Expr::opaque("matrix([1])")));
    CHECK_FALSE(
        proxima::is_evaluable(Expr::function("no_such_function", {Expr(1)})));

    // And eval_numeric still says why.
    CHECK(proxima::eval_numeric(Expr(x) + 1).error().message()
          == "no value for the symbol x");
}

TEST_CASE("the compiled form keeps 0.0 and -0.0 apart") {
    // Compiled folds repeated literals into one constant slot, and used to
    // match them with ==, which holds for 0.0 and -0.0. They are not
    // interchangeable: atan2(0.0, -1) is pi and atan2(-0.0, -1) is -pi, so the
    // compiled form of their sum answered 2 pi where eval_numeric answered 0.
    const Symbol x("x");
    const Expr signs = Expr::function("atan2", {Expr(0.0), Expr(-1)})
                       + Expr::function("atan2", {Expr(-0.0), Expr(-1)}) + Expr(x);

    const double walked = *proxima::eval_numeric(signs, {{"x", 0.0}});
    CHECK(walked == doctest::Approx(0.0));
    CHECK(proxima::Compiled(signs, x)(0.0) == doctest::Approx(walked));

    SUBCASE("and so does a reciprocal") {
        // 1/0.0 is inf and 1/-0.0 is -inf; merged, they add to inf rather
        // than to NaN.
        const Expr reciprocals = pow(Expr(0.0), Expr(-1)) * Expr(x)
                                 + pow(Expr(-0.0), Expr(-1)) * Expr(x);
        CHECK(std::isnan(*proxima::eval_numeric(reciprocals, {{"x", 1.0}})));
        CHECK(std::isnan(proxima::Compiled(reciprocals, x)(1.0)));
    }
}

TEST_CASE("bindings name a symbol by the Symbol or by its name") {
    const Symbol x("x");
    const Symbol y("y");

    // Compiled takes its variables as Symbols; the values can come the same way.
    CHECK(*proxima::eval_numeric(Expr(x) * Expr(y), {{x, 3.0}, {"y", 4.0}})
          == doctest::Approx(12.0));
    CHECK(proxima::Compiled(Expr(x) + Expr(y), x, {{y, 0.5}})(2.0)
          == doctest::Approx(2.5));
    CHECK(proxima::as_function(Expr(x) * Expr(y), x, {{y, 2.0}})(3.0)
          == doctest::Approx(6.0));
    CHECK(proxima::is_evaluable(Expr(x), {{x, 1.0}}));

    proxima::Bindings bindings{{x, 1.0}, {"x", 2.0}};
    CHECK(bindings.size() == 1u);
    CHECK(bindings.find(x)->second == 2.0); // The last value given wins.
    bindings.set(y, 5.0);
    CHECK(bindings.contains(y));
    CHECK(bindings.contains("y"));
    CHECK_FALSE(bindings.contains("z"));
    CHECK(bindings.find("z") == bindings.end());
    CHECK(*proxima::eval_numeric(Expr(x) + Expr(y), bindings)
          == doctest::Approx(7.0));

    std::string names;
    for (const auto &[name, value] : bindings) {
        names += name;
    }
    CHECK(names == "xy");
}

TEST_CASE("constants are recognised as Maxima spells them") {
    CHECK(*proxima::eval_numeric(proxima::pi())
          == doctest::Approx(std::numbers::pi));
    CHECK(*proxima::eval_numeric(proxima::e()) == doctest::Approx(std::numbers::e));
    CHECK(std::isinf(*proxima::eval_numeric(proxima::inf())));
    CHECK(*proxima::eval_numeric(proxima::minf()) < 0);
    CHECK(*proxima::eval_numeric(proxima::Expr::symbol("%phi"))
          == doctest::Approx(std::numbers::phi));
    CHECK(*proxima::eval_numeric(proxima::Expr::symbol("%gamma"))
          == doctest::Approx(std::numbers::egamma));

    SUBCASE("but an explicit binding still wins") {
        // So a symbol that happens to be named %e can be given a value.
        CHECK(*proxima::eval_numeric(proxima::e(), {{"%e", 10.0}})
              == doctest::Approx(10.0));
    }
}

TEST_CASE("what cannot be evaluated says so rather than guessing") {
    const Symbol x("x");
    // A Failure with Cause::Eval, never a number that looks plausible.
    const auto refused = [](const proxima::result<double> &value) {
        return !value.has_value()
               && proxima::cause_of(value.error()) == proxima::Cause::Eval;
    };

    SUBCASE("an unbound symbol") {
        CHECK(refused(proxima::eval_numeric(Expr(x))));
    }
    SUBCASE("a function with no numeric meaning here") {
        // Refusing beats returning something plausible for a function this does
        // not actually implement.
        CHECK(refused(
            proxima::eval_numeric(Expr::function("bessel_j", {Expr(0), Expr(1)}))));
    }
    SUBCASE("a relation") {
        CHECK(refused(proxima::eval_numeric(eq(Expr(1), Expr(1)))));
    }
    SUBCASE("an Opaque node") {
        // Maxima source this library never interpreted, so there is nothing
        // here that could evaluate it.
        CHECK(refused(proxima::eval_numeric(Expr::opaque("30!"))));
    }
    SUBCASE("and the message names the culprit") {
        const auto unbound = proxima::eval_numeric(Expr(x) + 1);
        REQUIRE_FALSE(unbound.has_value());
        CHECK(unbound.error().message().find("x") != std::string::npos);
        // And unwrap throws it as the exception the cause names.
        CHECK_THROWS_AS(proxima::unwrap(proxima::eval_numeric(Expr(x) + 1)),
                        proxima::EvalError);
    }
}

TEST_CASE("is_evaluable answers without throwing") {
    const Symbol x("x");
    CHECK_FALSE(proxima::is_evaluable(Expr(x)));
    CHECK(proxima::is_evaluable(Expr(x), {{"x", 1.0}}));
    CHECK(proxima::is_evaluable(proxima::pi()));
    CHECK_FALSE(proxima::is_evaluable(Expr::opaque("30!")));
}

TEST_CASE("as_function binds one variable for repeated use") {
    const Symbol x("x");
    const auto f = proxima::as_function(pow(Expr(x), 2), x);

    CHECK(f(0.0) == doctest::Approx(0.0));
    CHECK(f(3.0) == doctest::Approx(9.0));

    SUBCASE("with the other symbols fixed") {
        const Symbol a("a");
        const auto scaled
            = proxima::as_function(Expr(a) * Expr(x), x, {{"a", 10.0}});
        CHECK(scaled(2.5) == doctest::Approx(25.0));
    }
    SUBCASE("as the Compiled itself, which a std::function will hold") {
        // It used to be a std::function: an allocation, and an indirect call
        // per point.
        static_assert(std::same_as<decltype(f), const proxima::Compiled>);
        const std::function<double(double)> wrapped = f;
        CHECK(wrapped(4.0) == doctest::Approx(16.0));
    }
}

TEST_CASE("the version header reports the project version") {
    // Against project() in CMakeLists.txt, the one place the version is
    // written, rather than a copy of it here to update at every release.
    CHECK(std::string(proxima::version) == PROXIMA_PROJECT_VERSION);
    CHECK(std::string(proxima::version)
          == std::to_string(proxima::version_major) + "."
                 + std::to_string(proxima::version_minor) + "."
                 + std::to_string(proxima::version_patch));
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
        const Expr f = *Expr::parse(source);
        const proxima::Compiled compiled(f, x);

        for (const double at : {-2.5, -1.0, -0.25, 0.0, 0.5, 1.0, 3.75}) {
            CAPTURE(at);
            CHECK(same_number(compiled(at), *proxima::eval_numeric(f, {{"x", at}})));
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
        const proxima::Compiled compiled(f, x);

        for (const double base : {-3.0, -1.0, -0.5, 0.5, 1.0, 2.0}) {
            CAPTURE(base);
            CHECK(same_number(compiled(base), std::pow(base, exponent)));
        }
    }

    SUBCASE("including the degenerate cases") {
        CHECK(proxima::Compiled(pow(Expr(x), 0), x)(0.0)
              == doctest::Approx(std::pow(0.0, 0)));
        CHECK(std::isinf(proxima::Compiled(pow(Expr(x), -1), x)(0.0)));
    }

    SUBCASE("and an exponent too large for the specialisation still works") {
        const Expr big = pow(Expr(x), 200);
        CHECK(proxima::Compiled(big, x)(1.05)
              == doctest::Approx(std::pow(1.05, 200)));
    }
}

TEST_CASE("several variables, in the order given") {
    const Symbol x("x");
    const Symbol y("y");
    const std::vector<Symbol> variables{x, y};

    const proxima::Compiled compiled(*Expr::parse("x^2 + 2*y"), variables);
    REQUIRE(compiled.arity() == 2);
    CHECK(compiled.variable_names() == std::vector<std::string>{"x", "y"});

    const double point[] = {3.0, 5.0};
    CHECK(compiled(point) == doctest::Approx(19.0));

    SUBCASE("and the order is the caller's, not the expression's") {
        const std::vector<Symbol> reversed{y, x};
        const proxima::Compiled swapped(*Expr::parse("x^2 + 2*y"), reversed);
        const double same_values[] = {5.0, 3.0}; // y, x
        CHECK(swapped(same_values) == doctest::Approx(19.0));
    }
}

TEST_CASE("what shadows what") {
    const Symbol x("x");
    const Symbol e("%e");

    // A variable beats a binding, which beats a named constant.
    CHECK(proxima::Compiled(Expr::symbol("%e"), e)(7.0) == doctest::Approx(7.0));
    CHECK(proxima::Compiled(Expr::symbol("%e"), x, {{"%e", 5.0}})(0.0)
          == doctest::Approx(5.0));
    CHECK(proxima::Compiled(Expr::symbol("%e"), x)(0.0)
          == doctest::Approx(std::numbers::e));
}

TEST_CASE("errors surface at construction, not on every call") {
    // An unknown function or an unbound symbol is a property of the expression,
    // not of the point being evaluated, so it should be reported once.
    const Symbol x("x");

    CHECK_THROWS_AS(proxima::Compiled(Expr::symbol("y"), x), proxima::EvalError);
    CHECK_THROWS_AS(
        proxima::Compiled(Expr::function("bessel_j", {Expr(0), Expr(x)}), x),
        proxima::EvalError);
    CHECK_THROWS_AS(proxima::Compiled(eq(Expr(x), Expr(1)), x), proxima::EvalError);
    CHECK_THROWS_AS(proxima::Compiled(Expr::opaque("30!"), x), proxima::EvalError);

    SUBCASE("and compile reports the same as a value") {
        const auto unbound = proxima::compile(Expr::symbol("y"), x);
        REQUIRE_FALSE(unbound.has_value());
        CHECK(proxima::cause_of(unbound.error()) == proxima::Cause::Eval);
        CHECK(unbound.error().message().find("y") != std::string::npos);

        const auto square = proxima::compile(pow(Expr(x), 2), x);
        REQUIRE(square.has_value());
        CHECK((*square)(3.0) == 9.0);
    }

    SUBCASE("and the wrong number of values is refused") {
        const proxima::Compiled compiled(Expr(x), x);
        const double none[] = {0.0};
        CHECK_THROWS_AS(compiled(std::span<const double>(none, 0)),
                        proxima::EvalError);
    }
}

TEST_CASE("repeated literals share one constant slot") {
    // Not observable in the answer, only in the size — but it is the sort of
    // thing that silently stops working, so it is worth pinning.
    const Symbol x("x");
    const proxima::Compiled compiled(*Expr::parse("2*x + 2*x^2 + 2"), x);
    CHECK(compiled.size() > 0);
    CHECK(compiled(1.0) == doctest::Approx(6.0));
}

TEST_CASE("one compiled expression can be shared between threads") {
    // The working stack is thread-local, so no synchronisation is needed.
    const Symbol x("x");
    const proxima::Compiled compiled(*Expr::parse("sin(x)^2 + cos(x)^2"), x);

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
        = proxima::integrate(pow(Expr(x), 2) * proxima::sin(Expr(x)), x);
    REQUIRE(antiderivative.has_value());

    // 2x sin(x) + (2 - x^2) cos(x), evaluated at 1.
    const double expected = 2 * std::sin(1.0) + (2 - 1) * std::cos(1.0);
    CHECK(*proxima::eval_numeric(*antiderivative, {{"x", 1.0}})
          == doctest::Approx(expected));
}

TEST_CASE("a definite integral agrees with sampling its antiderivative") {
    // An end-to-end check that the symbolic and numeric halves agree:
    // F(1) - F(0) should equal what Maxima says the definite integral is.
    const Symbol x("x");
    const Expr integrand = pow(Expr(x), 3) + Expr(x);

    const auto exact = proxima::integrate(integrand, x, Expr(0), Expr(1));
    REQUIRE(exact.has_value());

    const auto antiderivative = proxima::integrate(integrand, x);
    REQUIRE(antiderivative.has_value());
    const auto f = proxima::as_function(*antiderivative, x);

    CHECK(f(1.0) - f(0.0) == doctest::Approx(*proxima::eval_numeric(*exact)));
}

} // TEST_SUITE("maxima")

// --- functions whose C library namesakes mean something else ----------------

TEST_CASE("mod and round are Maxima's, not the C library's") {
    // Both used to be the <cmath> function of the same name, which answers a
    // different question: std::fmod truncates towards zero and std::round
    // rounds halves away from zero. Every expectation below is the answer
    // Maxima itself gave.
    const auto mod = [](double a, double b) {
        return *proxima::eval_numeric(Expr::function("mod", {Expr(a), Expr(b)}));
    };
    const auto round = [](double value) {
        return *proxima::eval_numeric(Expr::function("round", {Expr(value)}));
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
        // A NaN cannot be an Expr, but it can still arrive through a binding.
        const Symbol x("x");
        CHECK(std::isnan(*proxima::eval_numeric(Expr::function("round", {Expr(x)}),
                                                {{"x", std::nan("")}})));
    }

    SUBCASE("the compiled form agrees, since it shares the function table") {
        const Symbol x("x");
        const proxima::Compiled compiled_mod(
            Expr::function("mod", {Expr(x), Expr(3)}), x);
        CHECK(compiled_mod(-7.0) == 2.0);
        CHECK(compiled_mod(-6.0) == 0.0);
        const proxima::Compiled compiled_round(Expr::function("round", {Expr(x)}),
                                               x);
        CHECK(compiled_round(2.5) == 2.0);
        CHECK(compiled_round(-2.5) == -2.0);
    }
}

TEST_SUITE("maxima") {

TEST_CASE("mod and round agree with Maxima across signs and halves") {
    // The builtins are the one place this library computes something Maxima
    // also computes, so they are checked against it directly — the way
    // test_integer.cpp checks bignum arithmetic — rather than against
    // expectations written by whoever wrote the code.
    proxima::Kernel kernel;

    // Integral values are sent as integers, which is how they would appear in
    // a closed form, and which sidesteps asking what mod(x, 0.0) means.
    const auto text = [](double value) {
        if (std::floor(value) == value && std::fabs(value) < 1e15) {
            return Expr(static_cast<long long>(value)).str();
        }
        return Expr(value).str();
    };
    const auto maxima = [&kernel](const std::string &source) {
        const auto answer = proxima::parse(source, kernel);
        REQUIRE_MESSAGE(answer.has_value(), source);
        return *proxima::eval_numeric(*answer);
    };

    const double dividends[] = {-7.5, -7, -6, -2.5, -0.5, 0, 0.5, 2.5, 6, 7, 7.5};
    const double divisors[] = {-3, -2, 2, 2.5, 3, 0};
    for (const double a : dividends) {
        for (const double b : divisors) {
            const std::string source = "mod(" + text(a) + ", " + text(b) + ")";
            CAPTURE(source);
            const double local
                = *proxima::eval_numeric(Expr::function("mod", {Expr(a), Expr(b)}));
            CHECK(same_number(local, maxima(source)));
        }
    }

    const double values[]
        = {-3.5, -2.6, -2.5, -1.5, -0.5, 0.5, 1.5, 2.4999, 2.5, 2.6, 3.5, 7};
    for (const double value : values) {
        const std::string source = "round(" + text(value) + ")";
        CAPTURE(source);
        const double local
            = *proxima::eval_numeric(Expr::function("round", {Expr(value)}));
        CHECK(same_number(local, maxima(source)));
    }
}

} // TEST_SUITE("maxima")

TEST_CASE("with() adds a binding to a copy, and leaves the original alone") {
    const Symbol h("h");
    const proxima::Bindings base{{"g", 9.81}};
    const proxima::Bindings here = base.with(h, 2.0).with("g", 10.0);

    CHECK_FALSE(base.contains(h));
    CHECK(base.find("g")->second == 9.81);
    CHECK(here.find(h)->second == 2.0);
    CHECK(here.find("g")->second == 10.0);
    CHECK(*proxima::eval_numeric(Expr::symbol("g") * h, here) == 20.0);
}

// --- the functions Maxima writes into its answers, and complex evaluation ---

namespace {

/// One builder per function numeric evaluation knows, with the name it has
/// there: the list the test below holds both sides to.
struct Built {
    std::string_view name;
    Expr expr;
};

std::vector<Built> every_builder(const Symbol &x, const Symbol &y) {
    namespace px = proxima;
    return {
        {"sin", px::sin(x)},
        {"cos", px::cos(x)},
        {"tan", px::tan(x)},
        {"sec", px::sec(x)},
        {"csc", px::csc(x)},
        {"cot", px::cot(x)},
        {"asin", px::asin(x)},
        {"acos", px::acos(x)},
        {"atan", px::atan(x)},
        {"asec", px::asec(x)},
        {"acsc", px::acsc(x)},
        {"acot", px::acot(x)},
        {"atan2", px::atan2(y, x)},
        {"sinh", px::sinh(x)},
        {"cosh", px::cosh(x)},
        {"tanh", px::tanh(x)},
        {"sech", px::sech(x)},
        {"csch", px::csch(x)},
        {"coth", px::coth(x)},
        {"asinh", px::asinh(x)},
        {"acosh", px::acosh(x)},
        {"atanh", px::atanh(x)},
        {"asech", px::asech(x)},
        {"acsch", px::acsch(x)},
        {"acoth", px::acoth(x)},
        {"exp", px::exp(x)},
        {"log", px::log(x)},
        {"sqrt", px::sqrt(x)},
        {"gamma", px::gamma(x)},
        {"factorial", px::factorial(x)},
        {"double_factorial", px::double_factorial(x)},
        {"erf", px::erf(x)},
        {"erfc", px::erfc(x)},
        {"abs", px::abs(x)},
        {"signum", px::signum(x)},
        {"floor", px::floor(x)},
        {"ceiling", px::ceiling(x)},
        {"round", px::round(x)},
        {"mod", px::mod(x, y)},
        {"max", px::max(x, y, 1)},
        {"min", px::min(x, y, 1)},
        {"realpart", px::realpart(x)},
        {"imagpart", px::imagpart(x)},
        {"conjugate", px::conjugate(x)},
        {"cabs", px::cabs(x)},
        {"carg", px::carg(x)},
    };
}

} // namespace

TEST_CASE("every function numeric evaluation knows has a builder, and the reverse") {
    const Symbol x("x");
    const Symbol y("y");

    std::set<std::string_view> built;
    for (const Built &builder : every_builder(x, y)) {
        CAPTURE(builder.name);
        CHECK(built.insert(builder.name).second); // Listed once.
        // exp and sqrt build %e^x and x^(1/2), Maxima having no node for
        // either; everything else is a call to the function of that name.
        if (builder.name != "exp" && builder.name != "sqrt") {
            REQUIRE(builder.expr.is(proxima::Kind::Function));
            CHECK(builder.expr.name() == builder.name);
        }
        CHECK(proxima::is_evaluable(builder.expr, {{x, 0.5}, {y, 2.0}}));
    }

    const std::span<const std::string_view> known = proxima::numeric_functions();
    const std::set<std::string_view> evaluable(known.begin(), known.end());
    CHECK(evaluable.size() == known.size()); // Named once.
    CHECK(built == evaluable);
}

TEST_CASE("the reciprocal functions, as Maxima defines them") {
    const auto at = [](const Expr &expr) { return *proxima::eval_numeric(expr); };
    constexpr double pi = std::numbers::pi;

    CHECK(at(proxima::sec(Expr(0))) == 1.0);
    CHECK(at(proxima::csc(Expr(pi / 2))) == 1.0);
    CHECK(at(proxima::cot(Expr(pi / 4))) == doctest::Approx(1.0));
    CHECK(at(proxima::sech(Expr(0))) == 1.0);
    CHECK(at(proxima::coth(Expr(1000))) == 1.0); // Not inf/inf.
    CHECK(at(proxima::asec(Expr(2))) == doctest::Approx(pi / 3));
    CHECK(at(proxima::acsc(Expr(2))) == doctest::Approx(pi / 6));
    // atan(1/x), so negative for a negative argument: -pi/4, not 3pi/4.
    CHECK(at(proxima::acot(Expr(-1))) == doctest::Approx(-pi / 4));
    CHECK(at(proxima::acot(Expr(0))) == doctest::Approx(pi / 2));
    CHECK(at(proxima::acoth(Expr(2))) == doctest::Approx(0.5493061443340549));

    // What made them necessary: Maxima's integral of tan(x).
    const Symbol x("x");
    CHECK(*proxima::eval_numeric(proxima::log(proxima::sec(x)), {{x, 1.0}})
          == doctest::Approx(-std::log(std::cos(1.0))));
}

TEST_CASE("gamma and the factorials") {
    const auto at = [](const Expr &expr) { return *proxima::eval_numeric(expr); };

    SUBCASE("exact where the answer is an integer a double holds") {
        CHECK(at(proxima::gamma(Expr(5))) == 24.0);
        CHECK(at(proxima::factorial(Expr(0))) == 1.0);
        CHECK(at(proxima::factorial(Expr(5))) == 120.0);
        CHECK(at(proxima::factorial(Expr(22))) == 1124000727777607680000.0);
        CHECK(at(proxima::double_factorial(Expr(7))) == 105.0);
        CHECK(at(proxima::double_factorial(Expr(8))) == 384.0);
        CHECK(at(proxima::double_factorial(Expr(0))) == 1.0);
        CHECK(at(proxima::double_factorial(Expr(-1))) == 1.0);
    }
    SUBCASE("continued past the integers, as Maxima continues them") {
        CHECK(at(proxima::gamma(Expr(0.5))) == doctest::Approx(1.772453850905516));
        CHECK(at(proxima::factorial(Expr(2.5)))
              == doctest::Approx(3.3233509704478426));
        CHECK(at(proxima::double_factorial(Expr(2.5)))
              == doctest::Approx(2.4070694561160435));
        CHECK(at(proxima::double_factorial(Expr(0.5)))
              == doctest::Approx(0.9628277824464171));
    }
    SUBCASE("overflowing to infinity") {
        CHECK(std::isinf(at(proxima::factorial(Expr(171)))));
        CHECK(std::isfinite(at(proxima::double_factorial(Expr(300)))));
        CHECK(std::isinf(at(proxima::double_factorial(Expr(301)))));
        CHECK(std::isinf(at(proxima::double_factorial(Expr(1000)))));
    }
    SUBCASE("5! and 7!! as Expr::parse reads them") {
        CHECK(at(*Expr::parse("5!")) == 120.0);
        CHECK(at(*Expr::parse("7!!")) == 105.0);
        CHECK(at(*Expr::parse("3!!!")) == 6.0); // (3!!)!, which is 3!.
    }
    CHECK(at(proxima::erfc(Expr(0.5))) == doctest::Approx(0.4795001221869535));
}

TEST_CASE("the compiled form knows the same functions") {
    const Symbol x("x");
    const Expr f = proxima::log(proxima::sec(x)) + proxima::gamma(x)
                   + proxima::double_factorial(x) + proxima::acot(x);
    const proxima::Compiled compiled(f, x);
    // At 2.5 and 4, sec(x) is negative and its log NaN, in both.
    for (const double value : {0.3, 1.0, 2.5, 4.0}) {
        CHECK(same_number(compiled(value), *proxima::eval_numeric(f, {{x, value}})));
    }
}

TEST_CASE("complex evaluation") {
    using Complex = std::complex<double>;
    const Symbol x("x");
    const Expr i = proxima::i();
    const auto at = [](const Expr &expr) { return *proxima::eval_complex(expr); };

    SUBCASE("%i is the imaginary unit") {
        CHECK(at(i) == Complex(0, 1));
        CHECK(at(-i) == Complex(0, -1));
        CHECK(at(2 + 3 * i) == Complex(2, 3));
        // Exactly: by multiplication, not by std::pow.
        CHECK(at(pow(i, 2)) == Complex(-1, 0));
        CHECK(at(pow(1 + i, -1)) == Complex(0.5, -0.5));
    }
    SUBCASE("functions take their principal values") {
        CHECK(at(proxima::sqrt(Expr(-4))) == Complex(0, 2));
        const Complex log_minus_one = at(proxima::log(Expr(-1)));
        CHECK(log_minus_one.real() == 0.0);
        CHECK(log_minus_one.imag() == doctest::Approx(std::numbers::pi));
        CHECK(at(proxima::exp(i * proxima::pi())).real() == doctest::Approx(-1.0));
        CHECK(at(proxima::cabs(3 + 4 * i)) == Complex(5, 0));
        CHECK(at(proxima::realpart(3 + 4 * i)) == Complex(3, 0));
        CHECK(at(proxima::imagpart(3 + 4 * i)) == Complex(4, 0));
        CHECK(at(proxima::conjugate(3 + 4 * i)) == Complex(3, -4));
    }
    SUBCASE("on a branch cut, the side Maxima takes") {
        // The upper side, except for asin, acos and atanh above 1.
        CHECK(at(proxima::asin(Expr(2))).imag() < 0);
        CHECK(at(proxima::asin(Expr(-2))).imag() > 0);
        CHECK(at(proxima::acos(Expr(2))).imag() > 0);
        CHECK(at(proxima::atanh(Expr(2))).imag() < 0);
        CHECK(at(proxima::acosh(Expr(0.5))).imag() > 0);
        CHECK(at(proxima::log(Expr(-2))).imag() > 0);
        // And 1/x taken for a real x is real, not a side of the cut chosen by
        // the sign of a zero: acsc(1/2) is asin(2).
        CHECK(at(proxima::acsc(Expr(0.5))) == at(proxima::asin(Expr(2))));
        CHECK(at(proxima::asec(Expr(-0.5))) == at(proxima::acos(Expr(-2))));
    }
    SUBCASE("a real expression gives exactly the real evaluator's number") {
        const Expr f = pow(x, 3) - proxima::sin(x) / x + proxima::gamma(x)
                       + proxima::atan2(x, Expr(-1)) + pow(x, Expr::rational(1, 3));
        for (const double value : {0.25, 1.0, 3.5}) {
            const Complex complex = *proxima::eval_complex(f, {{x, value}});
            CHECK(complex.real() == *proxima::eval_numeric(f, {{x, value}}));
            CHECK(complex.imag() == 0.0);
        }
    }
    SUBCASE("a function with no complex meaning takes real arguments only") {
        CHECK(at(proxima::floor(Expr(2.5))) == Complex(2, 0));
        const auto refused = proxima::eval_complex(proxima::floor(i));
        REQUIRE_FALSE(refused.has_value());
        CHECK(proxima::cause_of(refused.error()) == proxima::Cause::Eval);
        CHECK(refused.error().message().find("floor") != std::string::npos);
    }
    SUBCASE("the real evaluators say where to go instead") {
        const auto real = proxima::eval_numeric(-i);
        REQUIRE_FALSE(real.has_value());
        CHECK(real.error().message().find("eval_complex") != std::string::npos);
        CHECK_THROWS_AS(proxima::Compiled(i * x, x), proxima::EvalError);
    }
}

TEST_SUITE("maxima") {

TEST_CASE("every function evaluates as Maxima evaluates it") {
    // Checked against Maxima's float, over the complex numbers, so that where
    // a function leaves its real domain the branch taken is Maxima's too —
    // and not against numbers written here by whoever wrote the functions.
    using Complex = std::complex<double>;
    proxima::Kernel kernel;
    const Symbol x("x");
    const Symbol y("y");

    const auto close = [](Complex local, Complex maxima) {
        return std::abs(local - maxima) <= 1e-10 * std::max(1.0, std::abs(maxima));
    };
    // Maxima's number for `expr`, which must be one: a function left in the
    // answer would be evaluated here, and the check would check nothing.
    const auto maxima = [&kernel](const Expr &expr) -> std::optional<Complex> {
        const auto answer = proxima::to_float(expr, kernel);
        if (!answer) {
            return std::nullopt; // A pole, or a domain Maxima refuses.
        }
        CAPTURE(answer->str());
        REQUIRE_FALSE(proxima::any_of(
            *answer, [](const Expr &e) { return e.is(proxima::Kind::Function); }));
        return *proxima::eval_complex(*answer);
    };

    const double reals[] = {-2.5, -0.7, 0.3, 0.5, 2.0, 3.5};
    const Complex complexes[] = {{0.3, 0.5}, {-1.5, -0.25}};

    for (const Built &builder : every_builder(x, y)) {
        CAPTURE(builder.name);
        const auto at = [&](const Expr &value) {
            return proxima::replace(proxima::replace(builder.expr, x, value), y,
                                    Expr(2));
        };
        for (const double value : reals) {
            CAPTURE(value);
            const Expr point = at(Expr(value));
            const auto expected = maxima(point);
            if (!expected) {
                continue;
            }
            const Complex local = *proxima::eval_complex(point);
            CAPTURE(local);
            CAPTURE(*expected);
            CHECK(close(local, *expected));
            if (expected->imag() == 0.0) {
                CHECK(close(*proxima::eval_numeric(point), *expected));
            }
        }
        // A complex argument, for the functions with a complex meaning.
        for (const Complex z : complexes) {
            CAPTURE(z);
            const Expr point = at(Expr(z.real()) + Expr(z.imag()) * proxima::i());
            const auto local = proxima::eval_complex(point);
            if (!local) {
                continue; // floor and the rest, refused as they should be.
            }
            const auto expected = maxima(point);
            REQUIRE(expected.has_value());
            CAPTURE(*local);
            CAPTURE(*expected);
            CHECK(close(*local, *expected));
        }
    }
}

TEST_CASE("to_double asks Maxima for what it cannot evaluate itself") {
    proxima::Kernel kernel;
    const Symbol x("x");
    const Expr bessel = Expr::function("bessel_j", {Expr(0), x});

    REQUIRE_FALSE(proxima::eval_numeric(bessel, {{x, 1.0}}).has_value());
    CHECK(*proxima::to_double(bessel, {{x, 1.0}}, kernel)
          == doctest::Approx(0.7651976865579666));
    // The bindings reach inside an Opaque node, which only Maxima can read.
    CHECK(*proxima::to_double(Expr::opaque("x!") + 1, {{x, 5.0}}, kernel)
          == doctest::Approx(121.0));

    SUBCASE("what Maxima cannot make a number of is still refused") {
        const auto unbound = proxima::to_double(bessel + x, {}, kernel);
        REQUIRE_FALSE(unbound.has_value());
        CHECK(proxima::cause_of(unbound.error()) == proxima::Cause::Eval);
        CHECK(unbound.error().message().find("no value for the symbol x")
              != std::string::npos);
    }
    SUBCASE("a complex answer is no double, but it is a complex number") {
        const Expr root = proxima::sqrt(bessel);
        CHECK_FALSE(proxima::to_double(root, {{x, 3.0}}, kernel).has_value());
        const auto complex = proxima::to_complex(root, {{x, 3.0}}, kernel);
        REQUIRE(complex.has_value());
        CHECK(complex->real() == doctest::Approx(0.0));
        CHECK(complex->imag() == doctest::Approx(std::sqrt(0.26005195490193345)));
    }
}

} // TEST_SUITE("maxima")
