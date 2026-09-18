// The README's examples, compiled and checked. Each test mirrors one code
// block, with the values its comments promise; a README change that breaks
// an example, or an API change that breaks the README, fails here. This file
// exists because both happened: after the result type changed, the README's
// first example streamed a result<double> as if it were a number.

#include <doctest/doctest.h>

#include <proxima/assumptions.hpp>
#include <proxima/functions.hpp>
#include <proxima/kernel.hpp>
#include <proxima/mathml.hpp>
#include <proxima/numeric.hpp>
#include <proxima/ops.hpp>
#include <proxima/tex.hpp>

#include <fxt/monads/AndThen.hpp>
#include <fxt/monads/Transform.hpp>
#include <fxt/monads/ValueOr.hpp>
#include <fxt/utils/Lift.hpp>

#include <format>
#include <functional>
#include <sstream>
#include <string>

namespace px = proxima;

// --- Expressions -------------------------------------------------------------------

TEST_CASE("README: operators and infix text build the same expression") {
    const proxima::Symbol x("x"), y("y");

    proxima::Expr f = pow(x, 2) + 3 * x + 2;          // operators
    const proxima::Expr from_operators = f;
    f = *proxima::Expr::parse("x^2 + 3*x + 2");       // or infix text, no kernel
    CHECK(f == from_operators);
}

TEST_CASE("README: reading an expression is a match") {
    namespace node = proxima::node;
    const proxima::Symbol x("x");
    const auto what = [](const proxima::Expr &e) {
        return e.match(
            [](const node::Integer &n) { return "the integer " + n.value.to_string(); },
            [](const node::Symbol &s) { return "the symbol " + s.name; },
            [](const node::Sum &s) { return std::to_string(s.terms.size()) + " terms"; },
            [](const node::Call &c) { return "a call to " + c.head; },
            [](const auto &) { return std::string("something else"); });
    };
    CHECK(what(proxima::Expr(3)) == "the integer 3");
    CHECK(what(x) == "the symbol x");
    CHECK(what(x + 1) == "2 terms");
    CHECK(what(proxima::sin(x)) == "a call to sin");
    CHECK(what(proxima::Expr(2.5)) == "something else");
    CHECK(proxima::Expr(7).as_integer() == proxima::Integer(7));
}

TEST_CASE("README: rendering") {
    const proxima::Symbol x("x");
    const proxima::Expr e = (1 + x) / (x - 1);

    std::ostringstream out;
    out << e;
    CHECK(out.str() == "(1 + x)/(x - 1)");
    CHECK(out.str() == e.str());
    CHECK(std::format("{:tex}", e) == "\\frac{1 + x}{x - 1}");
    CHECK(std::format("{:tex}", e) == proxima::to_tex(e));
    CHECK(std::format("{:mathml}", e).starts_with("<math"));
}

TEST_CASE("README: assumptions are a value") {
    const proxima::Symbol x("x"), n("n"), k("k");
    const auto positive = proxima::assuming(gt(x, 0));
    const auto more = positive.with(gt(n, 0)).with(k, proxima::Feature::Integer);
    CHECK(more.facts().size() == 2);
    CHECK(more.declarations().size() == 1);
}

TEST_SUITE("maxima") {

TEST_CASE("README: the first example") {
    const px::Symbol x("x");
    const px::Expr f = pow(x, 2) * px::sin(x);

    const auto integral = px::integrate(f, x);
    REQUIRE(integral.has_value());
    std::ostringstream out;
    out << *integral;
    CHECK(out.str() == "cos(x)*(2 - x^2) + 2*x*sin(x)");
    CHECK(*px::eval_numeric(*integral, {{x, 1.0}}) == doctest::Approx(2.22324).epsilon(1e-5));
}

TEST_CASE("README: results chain, and a chain stops at the first failure") {
    const proxima::Symbol x("x");
    const proxima::Expr f = pow(x, 3);
    const auto chained = proxima::diff(f, x) | fxt::and_then(FXT_LIFT(proxima::factor))
                         | fxt::and_then(FXT_LIFT(proxima::expand));
    CHECK(chained == 3 * pow(x, 2));
}

TEST_CASE("README: numeric evaluation") {
    const proxima::Symbol x("x");
    const auto integral = proxima::integrate(pow(x, 2) * proxima::sin(x), x);
    REQUIRE(integral.has_value());

    CHECK(*proxima::eval_numeric(*integral, {{x, 1.0}}) == doctest::Approx(2.22324).epsilon(1e-5));
    CHECK_FALSE(proxima::is_evaluable(*integral));

    const proxima::Compiled f(*integral, x);
    CHECK(f(1.0) == doctest::Approx(2.22324).epsilon(1e-5));
    const auto g = proxima::as_function(*integral, x);
    CHECK(g(1.0) == f(1.0));
}

TEST_CASE("README: assumptions travel with the question") {
    proxima::Kernel kernel;
    const proxima::Symbol x("x");
    const auto positive = proxima::assuming(gt(x, 0));
    CHECK(*proxima::simplify(sqrt(pow(x, 2))) == proxima::abs(proxima::Expr(x)));
    CHECK(*proxima::simplify(sqrt(pow(x, 2)), positive) == proxima::Expr(x));
    CHECK(*proxima::simplify(sqrt(pow(x, 2)), {positive, kernel}) == proxima::Expr(x));
}

TEST_CASE("README: failure is an outcome") {
    const proxima::Symbol x("x");
    const auto area = proxima::integrate(proxima::exp(proxima::sin(x)), x);
    REQUIRE_FALSE(area.has_value());
    CHECK(area.error().message().starts_with("no closed form for"));
    CHECK(proxima::cause_of(area.error()) == proxima::Cause::NoClosedForm);
}

TEST_CASE("README: composing with FXT") {
    proxima::Kernel kernel;
    const proxima::Symbol x("x");
    const proxima::Expr f = pow(x, 4);
#ifdef __cpp_lib_bind_back
    const std::string answer
        = proxima::diff(f, x)
          | fxt::and_then(FXT_LIFT(proxima::factor))
          | fxt::and_then([&](const proxima::Expr &e) { return proxima::diff(e, x); })
          | fxt::and_then(std::bind_back(FXT_LIFT(proxima::expand), proxima::Env(kernel)))
          | fxt::transform(proxima::to_tex)
          | fxt::value_or(std::string("no answer"));
#else
    const std::string answer
        = proxima::diff(f, x)
          | fxt::and_then(FXT_LIFT(proxima::factor))
          | fxt::and_then([&](const proxima::Expr &e) { return proxima::diff(e, x); })
          | fxt::and_then([&](const proxima::Expr &e) { return proxima::expand(e, kernel); })
          | fxt::transform(proxima::to_tex)
          | fxt::value_or(std::string("no answer"));
#endif
    CHECK(answer == "12 x^{2}");
}

TEST_CASE("README: when Maxima needs a fact it has not been told") {
    const proxima::Symbol x("x"), n("n");
    const auto stuck = proxima::integrate(pow(x, n), x);
    REQUIRE_FALSE(stuck.has_value());
    CHECK(proxima::cause_of(stuck.error()) == proxima::Cause::NeedsAssumption);
    CHECK(stuck.error().message().find("Is n equal to -1?") != std::string::npos);

    const auto known = proxima::integrate(pow(x, n), x, proxima::assuming(gt(n, 0)));
    REQUIRE(known.has_value());
    CHECK(known->str() == "x^(1 + n)/(1 + n)");
}

} // TEST_SUITE("maxima")
