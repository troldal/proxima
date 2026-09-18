// The operations. Split in two: the parts that need no kernel are in the
// default unit run, the rest are integration tests against real Maxima.

#include <doctest/doctest.h>

#include <fxt/monads/AndThen.hpp>
#include <fxt/monads/Transform.hpp>
#include <fxt/monads/ValueOr.hpp>
#include <fxt/utils/Lift.hpp>

#include <proxima/context.hpp>
#include <proxima/errors.hpp>
#include <proxima/expr.hpp>
#include <proxima/functions.hpp>
#include <proxima/ops.hpp>
#include <proxima/symbol.hpp>
#include <proxima/tex.hpp>

#include <cmath>
#include <concepts>
#include <functional>
#include <numbers>
#include <numeric>
#include <span>
#include <string>
#include <vector>

using proxima::Expr;
using proxima::Kind;
using proxima::Symbol;

// --- Maxima-free ----------------------------------------------------------

TEST_CASE("contains finds a symbol anywhere in an expression") {
    const Symbol x("x");
    const Symbol y("y");

    CHECK(proxima::contains(Expr(x), x));
    CHECK_FALSE(proxima::contains(Expr(y), x));
    CHECK(proxima::contains(pow(Expr(x) + 1, 2) * Expr(y), x));
    CHECK(proxima::contains(proxima::sin(proxima::cos(Expr(x))), x));
    CHECK_FALSE(proxima::contains(Expr(1) / Expr(3), x));
    // A symbol whose name merely starts the same is a different symbol.
    CHECK_FALSE(proxima::contains(Expr::symbol("xy"), x));
}

TEST_CASE("contains looks inside Opaque text as well") {
    // solve rejects an answer that still mentions its unknown — `[x = sin(x)]`
    // is Maxima saying it could not finish. A mention hidden in unmodelled text
    // used to get through that check.
    const Symbol x("x");

    CHECK(proxima::contains(Expr::opaque("sin(x) + 1"), x));
    CHECK(proxima::contains(Expr::function("f", {Expr::opaque("x^2")}), x));
    CHECK_FALSE(proxima::contains(Expr::opaque("sin(y) + 1"), x));

    SUBCASE("as a whole identifier, not part of a longer one") {
        CHECK(proxima::contains(Expr::opaque("2*x+1"), x));
        CHECK_FALSE(proxima::contains(Expr::opaque("xy + x_1 + %x + x2"), x));
    }

    SUBCASE("and not inside a string literal") {
        CHECK_FALSE(proxima::contains(Expr::opaque(R"("x marks the spot")"), x));
        CHECK_FALSE(proxima::contains(Expr::opaque(R"("say \"x\" twice")"), x));
        CHECK(proxima::contains(Expr::opaque(R"(concat("a", x))"), x));
    }

    SUBCASE("while a name that is not a plain identifier is found as written") {
        // Maxima source spells the symbol `x y` with a backslash.
        const Symbol spaced("x y");
        CHECK(proxima::contains(Expr::opaque(R"(f(x\ y))"), spaced));
        CHECK_FALSE(proxima::contains(Expr::opaque("f(x, y)"), spaced));
    }
}

TEST_CASE("function builders produce uninterpreted applications") {
    const Symbol x("x");

    CHECK(proxima::sin(Expr(x)).str() == "sin(x)");
    CHECK(proxima::log(Expr(x)).str() == "log(x)");
    // Nothing is evaluated locally; sin(0) stays sin(0) until Maxima is asked.
    CHECK(proxima::sin(Expr(0)).kind() == Kind::Function);

    SUBCASE("except the two Maxima has no node for either") {
        // exp is %e^x and sqrt is x^(1/2), internally in Maxima as well, so
        // building them that way keeps the representations in step.
        CHECK(proxima::exp(Expr(x)).kind() == Kind::Pow);
        CHECK(proxima::exp(Expr(x)).str() == "%e^x");
        CHECK(proxima::sqrt(Expr(x)).kind() == Kind::Pow);
        // Parenthesised, because x^1/2 would parse as (x^1)/2.
        CHECK(proxima::sqrt(Expr(x)).str() == "x^(1/2)");
    }
}

TEST_CASE("constants are spelled as Maxima names them") {
    CHECK(proxima::pi().str() == "%pi");
    CHECK(proxima::e().str() == "%e");
    CHECK(proxima::inf().str() == "inf");
}

TEST_CASE("lhs and rhs take a relation apart, locally") {
    const Symbol x("x");
    const Expr relation = proxima::le(Expr(x) + 1, Expr(3));
    CHECK(proxima::lhs(relation) == Expr(x) + 1);
    CHECK(proxima::rhs(relation) == Expr(3));
    // Maxima's lhs would return x + 1 itself, hiding the mistake.
    CHECK_THROWS_AS(static_cast<void>(proxima::lhs(Expr(x) + 1)), proxima::Error);
    CHECK_THROWS_AS(static_cast<void>(proxima::rhs(Expr(x))), proxima::Error);
}

TEST_CASE("derivative builds the noun a differential equation is written with") {
    const Symbol x("x");
    const Symbol y("y");
    // The order is spelled out, as Maxima's own 'diff(y, x) reads back.
    CHECK(proxima::derivative(Expr(y), x)
          == Expr::function("'diff", {Expr(y), Expr(x), Expr(1)}));
    CHECK(proxima::derivative(Expr(y), x, 2).arg(2) == Expr(2));
}

namespace {
template <typename T>
concept BuildsFrom = requires(const T &value) {
    proxima::abs(value);
    proxima::floor(value);
    proxima::sqrt(value);
};

template <typename T>
concept PowersFrom = requires(const T &value) { proxima::pow(value, value); };

template <typename T>
concept GcdFrom = requires(const T &value) { proxima::gcd(value, value); };
} // namespace

TEST_CASE("Proxima functions named like <cmath> ones take no plain numbers") {
    // Taking `const Expr &` or `const Integer &`, proxima::abs was a candidate for
    // abs(-3), and proxima::pow for pow(2, 3), through the implicit constructors:
    // losing overload resolution, but one added overload from an ambiguity.
    static_assert(BuildsFrom<Expr>);
    static_assert(BuildsFrom<Symbol>);
    static_assert(!BuildsFrom<int>);
    static_assert(!BuildsFrom<double>);

    static_assert(PowersFrom<Expr>);
    static_assert(PowersFrom<Symbol>);
    static_assert(!PowersFrom<int>);
    static_assert(!PowersFrom<double>);

    static_assert(GcdFrom<proxima::Integer>);
    static_assert(!GcdFrom<int>);
    static_assert(!GcdFrom<long long>);

    using namespace proxima; // NOLINT: the situation being guarded against.
    using std::abs;
    using std::floor;
    using std::gcd;
    using std::pow;
    static_assert(std::same_as<decltype(abs(-3)), int>);
    static_assert(std::same_as<decltype(pow(2.0, 3)), double>);
    CHECK(abs(-3) == 3);
    CHECK(floor(2.5) == 2.0);
    CHECK(gcd(12, 18) == 6);
    CHECK(abs(Symbol("x")) == Expr::function("abs", {Expr::symbol("x")}));

    SUBCASE("while one side may still be a plain number") {
        const Symbol x("x");
        CHECK(pow(x, 2) == Expr::pow(Expr(x), Expr(2)));
        CHECK(pow(2, Expr(x)) == Expr::pow(Expr(2), Expr(x)));
        CHECK(proxima::gcd(proxima::Integer(12), 18) == proxima::Integer(6));
        CHECK(proxima::gcd(30, proxima::Integer(12)) == proxima::Integer(6));
    }
}

TEST_CASE("every builder is spelled as Maxima spells the function") {
    const Symbol x("x");
    CHECK(proxima::tanh(x).str() == "tanh(x)");
    CHECK(proxima::asinh(x).str() == "asinh(x)");
    CHECK(proxima::acosh(x).str() == "acosh(x)");
    CHECK(proxima::atanh(x).str() == "atanh(x)");
    CHECK(proxima::erf(x).str() == "erf(x)");
    CHECK(proxima::floor(x).str() == "floor(x)");
    CHECK(proxima::ceiling(x).str() == "ceiling(x)");
    CHECK(proxima::signum(x).str() == "signum(x)");
    CHECK(proxima::minf().str() == "minf");
}

// --- Against a real kernel -------------------------------------------------

TEST_SUITE("maxima") {

TEST_CASE("the shared kernel is one process, reused") {
    CHECK(&proxima::shared_kernel() == &proxima::shared_kernel());

    // A binding made by one call is visible to the next, which is the
    // observable consequence of there being a single long-lived process.
    // Note this goes through Kernel::eval, not proxima::parse: parsing is only
    // parsing, and deliberately does not evaluate what it reads.
    REQUIRE(proxima::shared_kernel().eval("shared_probe: 11").has_value());
    CHECK(proxima::shared_kernel().eval("shared_probe^2").value() == "121");
}

TEST_CASE("differentiation") {
    const Symbol x("x");
    CHECK(*proxima::diff(pow(Expr(x), 2), x) == Expr(2) * Expr(x));
    CHECK(*proxima::diff(proxima::sin(Expr(x)), x) == proxima::cos(Expr(x)));

    SUBCASE("to higher order") {
        CHECK(*proxima::diff(proxima::sin(Expr(x)), x, 2) == -proxima::sin(Expr(x)));
        CHECK(*proxima::diff(pow(Expr(x), 3), x, 3) == Expr(6));
    }
    SUBCASE("with respect to an absent symbol is zero") {
        CHECK(*proxima::diff(Expr::symbol("y"), x) == Expr(0));
    }
}

TEST_CASE("algebraic rearrangement") {
    const Symbol x("x");

    CHECK(*proxima::expand(pow(Expr(x) + 1, 2))
          == Expr(1) + 2 * Expr(x) + pow(Expr(x), 2));
    CHECK(*proxima::ratsimp((pow(Expr(x), 2) - 1) / (Expr(x) - 1)) == Expr(x) + 1);
    // simplify is the older name for the same thing.
    CHECK(*proxima::simplify((pow(Expr(x), 2) - 1) / (Expr(x) - 1)) == Expr(x) + 1);
    // And it knows no identities: sin(x)^2 + cos(x)^2 is trigsimp's to reduce.
    CHECK(proxima::ratsimp(pow(proxima::sin(Expr(x)), 2) + pow(proxima::cos(Expr(x)), 2))->kind() == Kind::Add);
    CHECK(*proxima::subst(pow(Expr(x), 2) + 1, x, Expr(5)) == Expr(26));

    SUBCASE("factoring returns a product") {
        const Expr factored = *proxima::factor(pow(Expr(x), 2) - 1);
        CHECK(factored.kind() == Kind::Mul);
        // And expanding it again recovers the original.
        CHECK(*proxima::expand(factored) == pow(Expr(x), 2) - 1);
    }
}

TEST_CASE("integration") {
    const Symbol x("x");

    const auto integral = proxima::integrate(pow(Expr(x), 2), x);
    REQUIRE(integral.has_value());
    CHECK(*integral == pow(Expr(x), 3) / Expr(3));

    SUBCASE("a definite integral evaluates exactly") {
        const auto area = proxima::integrate(pow(Expr(x), 2), x, Expr(0), Expr(1));
        REQUIRE(area.has_value());
        CHECK(*area == Expr::rational(1, 3));
    }

    SUBCASE("differentiating the result recovers the integrand") {
        const auto antiderivative
            = proxima::integrate(pow(Expr(x), 2) * proxima::sin(Expr(x)), x);
        REQUIRE(antiderivative.has_value());
        CHECK(*proxima::simplify(*proxima::diff(*antiderivative, x))
              == pow(Expr(x), 2) * proxima::sin(Expr(x)));
    }

    SUBCASE("no closed form is a Failure, not an exception") {
        // Maxima signals this by returning the integral unevaluated rather
        // than by erroring, so the noun form is what gets recognised.
        const auto hopeless = proxima::integrate(proxima::exp(proxima::sin(Expr(x))), x);
        REQUIRE_FALSE(hopeless.has_value());
        CHECK(hopeless.error().message().find("no closed form")
              != std::string::npos);
    }

    SUBCASE("a genuine Maxima error is also a Failure") {
        // A divergent definite integral is something Maxima *errors* on,
        // rather than handing back unevaluated.
        const auto bad = proxima::integrate(Expr(1) / Expr(x), x, Expr(0), Expr(1));
        REQUIRE_FALSE(bad.has_value());
        CHECK(bad.error().message().find("divergent") != std::string::npos);
    }
}

TEST_CASE("limits") {
    const Symbol x("x");

    const auto sinc = proxima::limit(proxima::sin(Expr(x)) / Expr(x), x, Expr(0));
    REQUIRE(sinc.has_value());
    CHECK(*sinc == Expr(1));

    SUBCASE("one-sided limits differ from the two-sided one") {
        const auto above
            = proxima::limit(Expr(1) / Expr(x), x, Expr(0), proxima::Side::FromAbove);
        const auto below
            = proxima::limit(Expr(1) / Expr(x), x, Expr(0), proxima::Side::FromBelow);
        REQUIRE(above.has_value());
        REQUIRE(below.has_value());
        CHECK(above->str() == "inf");
        CHECK(below->str() == "minf");
    }

    SUBCASE("at infinity") {
        const auto decay = proxima::limit(Expr(1) / Expr(x), x, proxima::inf());
        REQUIRE(decay.has_value());
        CHECK(*decay == Expr(0));
    }

    SUBCASE("a limit that does not exist is a Failure, however Maxima says so") {
        // `ind`: bounded, but never settling. It used to come back as a
        // success holding the symbol ind, which a caller checking only the
        // std::expected took for an answer.
        const auto oscillating = proxima::limit(proxima::sin(Expr(1) / Expr(x)), x, Expr(0));
        REQUIRE_FALSE(oscillating.has_value());
        CHECK(oscillating.error().message().find("does not exist") != std::string::npos);
        CHECK(oscillating.error().message().find("bounded") != std::string::npos);

        const auto step = proxima::limit(proxima::abs(Expr(x)) / Expr(x), x, Expr(0));
        CHECK_FALSE(step.has_value());

        // `und`: undefined. Already a Failure.
        const auto undefined = proxima::limit(proxima::exp(Expr(1) / Expr(x)), x, Expr(0));
        REQUIRE_FALSE(undefined.has_value());
        CHECK(undefined.error().message().find("does not exist") != std::string::npos);

        // From one side the step has a value after all.
        const auto right
            = proxima::limit(proxima::abs(Expr(x)) / Expr(x), x, Expr(0), proxima::Side::FromAbove);
        REQUIRE(right.has_value());
        CHECK(*right == Expr(1));
    }

    SUBCASE("an infinite limit is still a value") {
        // From both sides 1/x grows without a sign: Maxima's complex infinity.
        const auto unsigned_infinity = proxima::limit(Expr(1) / Expr(x), x, Expr(0));
        REQUIRE(unsigned_infinity.has_value());
        CHECK(unsigned_infinity->str() == "infinity");
    }
}

TEST_CASE("solving") {
    const Symbol x("x");

    const auto roots = proxima::solve(eq(pow(Expr(x), 2), Expr(1)), x);
    REQUIRE(roots.has_value());
    REQUIRE(roots->size() == 2);
    CHECK((*roots)[0] == Expr(-1));
    CHECK((*roots)[1] == Expr(1));

    SUBCASE("a linear equation has one solution") {
        const auto one = proxima::solve(eq(2 * Expr(x) + 1, Expr(0)), x);
        REQUIRE(one.has_value());
        REQUIRE(one->size() == 1);
        CHECK((*one)[0] == Expr::rational(-1, 2));
    }

    SUBCASE("an equation Maxima cannot rearrange is a Failure") {
        // Maxima returns [x = sin(x)] here: an equation, but not a solution.
        // Accepting it would hand the caller something useless that looks like
        // an answer.
        const auto stuck = proxima::solve(eq(proxima::sin(Expr(x)), Expr(x)), x);
        REQUIRE_FALSE(stuck.has_value());
        CHECK(stuck.error().message().find("did not solve") != std::string::npos);
    }

    SUBCASE("and so is one it never rearranges at all") {
        // [0 = x^5 - x - 1]: the unknown is not even on the left.
        const auto quintic
            = proxima::solve(eq(pow(Expr(x), 5) - Expr(x) - 1, Expr(0)), x);
        CHECK_FALSE(quintic.has_value());
    }

    SUBCASE("no solutions is an answer, not a failure") {
        const auto none = proxima::solve(eq(Expr(1), Expr(0)), x);
        REQUIRE(none.has_value());
        CHECK(none->empty());
    }
}

TEST_CASE("solving a system") {
    const Symbol x("x");
    const Symbol y("y");
    const std::vector<Symbol> unknowns{x, y};

    const std::vector<Expr> linear{eq(Expr(x) + Expr(y), Expr(3)),
                                   eq(Expr(x) - Expr(y), Expr(1))};

    const auto solutions = proxima::solve(linear, unknowns);
    REQUIRE(solutions.has_value());
    REQUIRE(solutions->size() == 1);
    REQUIRE(solutions->front().size() == 2);
    CHECK(solutions->front()[0] == Expr(2)); // x
    CHECK(solutions->front()[1] == Expr(1)); // y

    SUBCASE("values follow the order the unknowns were asked for") {
        // Maxima answers in whatever order it likes; the correspondence is
        // established by name, not by position.
        const std::vector<Symbol> reversed{y, x};
        const auto swapped = proxima::solve(linear, reversed);
        REQUIRE(swapped.has_value());
        REQUIRE(swapped->front().size() == 2);
        CHECK(swapped->front()[0] == Expr(1)); // y
        CHECK(swapped->front()[1] == Expr(2)); // x
    }

    SUBCASE("a system with several solutions returns each of them") {
        const std::vector<Expr> circle{
            eq(pow(Expr(x), 2) + pow(Expr(y), 2), Expr(1)),
            eq(Expr(y), Expr(x))};
        const auto both = proxima::solve(circle, unknowns);
        REQUIRE(both.has_value());
        CHECK(both->size() == 2);
        // On this circle the two coordinates are equal in both solutions.
        for (const proxima::Solution &solution : *both) {
            REQUIRE(solution.size() == 2);
            CHECK(solution[0] == solution[1]);
        }
    }

    SUBCASE("no solution is an answer, not a failure") {
        const std::vector<Expr> contradictory{eq(Expr(x), Expr(1)),
                                              eq(Expr(x), Expr(2))};
        const std::vector<Symbol> just_x{x};
        const auto none = proxima::solve(contradictory, just_x);
        REQUIRE(none.has_value());
        CHECK(none->empty());
    }
}

TEST_CASE("a single unknown works through the system form too") {
    // Maxima flattens the result when there is one unknown — solve([x^2=1],[x])
    // gives [x = -1, x = 1] rather than [[x = -1], [x = 1]] — so the shape has
    // to be detected rather than assumed from the number of unknowns.
    const Symbol x("x");
    const std::vector<Expr> equations{eq(pow(Expr(x), 2), Expr(1))};
    const std::vector<Symbol> unknowns{x};

    const auto solutions = proxima::solve(equations, unknowns);
    REQUIRE(solutions.has_value());
    REQUIRE(solutions->size() == 2);
    CHECK(solutions->at(0).size() == 1);
    CHECK(solutions->at(0)[0] == Expr(-1));
    CHECK(solutions->at(1)[0] == Expr(1));
}

TEST_CASE("an underdetermined system solves parametrically") {
    // One equation, two unknowns: Maxima introduces a free parameter named %r1,
    // %r2 and so on. That is a value like any other, and not among the
    // unknowns, so it is not grounds for rejection.
    const Symbol x("x");
    const Symbol y("y");
    const std::vector<Expr> equations{eq(Expr(x) + Expr(y), Expr(3))};
    const std::vector<Symbol> unknowns{x, y};

    const auto solutions = proxima::solve(equations, unknowns);
    REQUIRE(solutions.has_value());
    REQUIRE(solutions->size() == 1);

    // y is the parameter and x is 3 minus it, so the two still sum to 3.
    const proxima::Solution &solution = solutions->front();
    REQUIRE(solution.size() == 2);
    CHECK(*proxima::simplify(solution[0] + solution[1]) == Expr(3));
}

TEST_CASE("a system Maxima cannot solve is a Failure") {
    const Symbol x("x");
    const Symbol y("y");
    const std::vector<Expr> equations{eq(proxima::sin(Expr(x)), Expr(x)),
                                      eq(Expr(y), Expr(1))};
    const std::vector<Symbol> unknowns{x, y};

    CHECK_FALSE(proxima::solve(equations, unknowns).has_value());
}

TEST_CASE("solving a system rejects degenerate arguments") {
    const Symbol x("x");
    const std::vector<Expr> equations{eq(Expr(x), Expr(1))};
    const std::vector<Symbol> unknowns{x};

    CHECK_FALSE(proxima::solve(equations, std::span<const Symbol>{}).has_value());
    CHECK_FALSE(proxima::solve(std::span<const Expr>{}, unknowns).has_value());
}

TEST_CASE("parsing delegates to Maxima's own parser") {
    const auto parsed = proxima::parse("x^2 + 3*x + 2");
    REQUIRE(parsed.has_value());
    CHECK(*parsed == pow(Expr::symbol("x"), 2) + 3 * Expr::symbol("x") + 2);

    SUBCASE("exactness survives") {
        const auto third = proxima::parse("1/3");
        REQUIRE(third.has_value());
        CHECK(*third == Expr::rational(1, 3));
    }
    SUBCASE("malformed input is a Failure, not an exception") {
        const auto broken = proxima::parse("this is not maxima ][");
        CHECK_FALSE(broken.has_value());
    }
    SUBCASE("a quote in the source does not break the call") {
        // The source is embedded in a Maxima string literal, so it has to be
        // escaped on the way in.
        const auto quoted = proxima::parse("\"a string\"");
        CHECK(quoted.has_value());
    }
}

TEST_CASE("an operation with no ordinary failure mode throws instead") {
    // diff cannot sensibly fail, so a Maxima error there is exceptional. An
    // Opaque that Maxima cannot parse is the surest way to provoke one.
    //
    // (This used to use Symbol("2") as the variable. That worked only because
    // the expression was rendered to text, which turned the symbol into the
    // number 2 on the way. It now travels as the symbol it is, which Maxima
    // is perfectly happy to differentiate with respect to.)
    const auto refused = proxima::diff(Expr::opaque("(1"), Symbol("x"));
    REQUIRE_FALSE(refused.has_value());
    CHECK(proxima::cause_of(refused.error()) == proxima::Cause::MaximaError);
    // For a caller who would rather catch: the same outcome as the exception
    // its cause names.
    CHECK_THROWS_AS(proxima::unwrap(proxima::diff(Expr::opaque("(1"), Symbol("x"))),
                    proxima::MaximaError);
}

TEST_CASE("results compose with FXT's adaptors, as the README shows") {
    // Proxima has no pipe adaptors of its own: the result type is FXT's, so
    // these idioms are the composition story, and they had better compile.
    const Symbol x("x");
    const Expr f = pow(Expr(x), 3);

    const std::string answer
        = proxima::diff(f, x)
          | fxt::and_then(FXT_LIFT(proxima::factor))
          | fxt::and_then([&](const Expr &e) { return proxima::diff(e, x); })
          | fxt::and_then([&](const Expr &e) { return proxima::expand(e, proxima::shared_kernel()); })
          | fxt::transform(proxima::to_tex)
          | fxt::value_or(std::string("no answer"));
    CHECK(answer == "6 x");

#ifdef __cpp_lib_bind_back
    // std::bind_back binds the variable without a lambda, where the standard
    // library has it (libstdc++ 14, MSVC's STL).
    const auto bound = proxima::diff(f, x) | fxt::and_then(std::bind_back(FXT_LIFT(proxima::diff), x));
    CHECK(bound == 6 * Expr(x));
#endif

    // A chain stops at the first failure, and the failure that stopped it is
    // the one that comes out.
    const auto stopped = proxima::diff(Expr::opaque("(1"), x)
                         | fxt::and_then(FXT_LIFT(proxima::factor))
                         | fxt::transform(proxima::to_tex);
    REQUIRE_FALSE(stopped.has_value());
    CHECK(proxima::cause_of(stopped.error()) == proxima::Cause::MaximaError);

    // And a chain can start from a plain expression by wrapping it once.
    const auto started = proxima::result<Expr>{f} | fxt::and_then(FXT_LIFT(proxima::factor));
    CHECK(started == f);
}

TEST_CASE("results are canonical expressions, not text") {
    // The whole point of the layer: what comes back is an Expr that can be fed
    // straight into the next operation.
    const Symbol x("x");
    const Expr chained
        = *proxima::expand(*proxima::factor(*proxima::diff(pow(Expr(x), 3) + pow(Expr(x), 2), x)));
    CHECK(chained == 3 * pow(Expr(x), 2) + 2 * Expr(x));
}

TEST_CASE("is asks a predicate under the assumptions in force") {
    // A name no other test assumes anything about.
    const Expr a = Expr::symbol("mx_is_probe");

    CHECK(*proxima::is(proxima::gt(a, 0)) == proxima::Truth::Unknown);
    {
        proxima::Context context;
        context.assume(proxima::gt(a, 0));
        // The same question as above, so a stale cached Unknown would show.
        CHECK(*proxima::is(proxima::gt(a, 0)) == proxima::Truth::True);
        CHECK(*proxima::is(proxima::lt(a, 0)) == proxima::Truth::False);
    }
    CHECK(*proxima::is(proxima::gt(a, 0)) == proxima::Truth::Unknown);
    CHECK(*proxima::is(proxima::gt(Expr(2), Expr(1))) == proxima::Truth::True);
}

TEST_CASE("series, trigonometric, radical and partial-fraction rearrangement") {
    const Symbol x("x");

    CHECK(*proxima::taylor(proxima::sin(Expr(x)), x, Expr(0), 5)
          == Expr(x) + Expr::rational(-1, 6) * pow(Expr(x), 3)
                 + Expr::rational(1, 120) * pow(Expr(x), 5));
    CHECK(*proxima::trigsimp(pow(proxima::sin(Expr(x)), 2) + pow(proxima::cos(Expr(x)), 2)) == Expr(1));
    CHECK(*proxima::trigexpand(proxima::sin(2 * Expr(x))) == 2 * proxima::cos(Expr(x)) * proxima::sin(Expr(x)));
    CHECK(*proxima::radcan(proxima::exp(2 * proxima::log(Expr(x)))) == pow(Expr(x), 2));
    CHECK(*proxima::partfrac(1 / (pow(Expr(x), 2) - 1), x)
          == Expr::rational(1, 2) * pow(Expr(x) - 1, -1)
                 + Expr::rational(-1, 2) * pow(Expr(x) + 1, -1));
}

TEST_CASE("float and coeff") {
    const Symbol x("x");

    CHECK(*proxima::to_float(proxima::pi() + Expr(x)) == Expr(std::numbers::pi) + Expr(x));
    CHECK(proxima::to_float(Expr::rational(1, 3))->kind() == Kind::Real);

    const Expr p = 3 * pow(Expr(x), 2) + 2 * Expr(x) + 5;
    CHECK(*proxima::coeff(p, Expr(x), 2) == Expr(3));
    CHECK(*proxima::coeff(p, Expr(x)) == Expr(2));
    CHECK(*proxima::coeff(p, Expr(x), 0) == Expr(5));
    // Taken as it stands, not expanded.
    CHECK(*proxima::coeff(pow(Expr(x) + 1, 2), Expr(x)) == Expr(0));
}

TEST_CASE("sums and products in closed form") {
    const Symbol k("k");
    const Expr n = Expr::symbol("n");

    const auto triangular = proxima::sum(Expr(k), k, Expr(1), n);
    REQUIRE(triangular.has_value());
    CHECK(*proxima::expand(*triangular)
          == Expr::rational(1, 2) * n + Expr::rational(1, 2) * pow(n, 2));
    CHECK(proxima::sum(Expr(k), k, Expr(1), Expr(5)).value() == Expr(15));
    CHECK(proxima::sum(pow(Expr(2), -Expr(k)), k, Expr(0), proxima::inf()).value() == Expr(2));
    CHECK(proxima::product(Expr(k), k, Expr(1), Expr(5)).value() == Expr(120));

    SUBCASE("and a Failure where there is none") {
        CHECK_FALSE(proxima::sum(Expr::function("f", {Expr(k)}), k, Expr(1), n).has_value());
        CHECK_FALSE(proxima::product(Expr(k), k, Expr(1), n).has_value());
    }
}

TEST_CASE("real roots: counted, isolated and found") {
    const Symbol x("x");
    const Expr y = Expr::symbol("y");

    CHECK(*proxima::nroots(pow(Expr(x), 3) - Expr(x), Expr(-2), Expr(2)) == 3u);
    // The interval is (low, high]: the root at -1 is outside it.
    CHECK(*proxima::nroots(pow(Expr(x), 2) - 1, Expr(-1), Expr(1)) == 1u);
    CHECK(*proxima::nroots(pow(Expr(x), 2) - 1) == 2u);
    CHECK(proxima::cause_of(proxima::nroots(Expr(x) * y - 1).error())
          == proxima::Cause::MaximaError);

    CHECK(*proxima::realroots(pow(Expr(x), 2) - 1) == std::vector<Expr>{Expr(-1), Expr(1)});
    CHECK(proxima::realroots(pow(Expr(x), 2) + 1)->empty());
    const std::vector<Expr> cube_root = *proxima::realroots(pow(Expr(x), 3) - 2);
    REQUIRE(cube_root.size() == 1);
    REQUIRE(cube_root[0].is(Kind::Rational));
    CHECK(cube_root[0].numerator().to_double() / cube_root[0].denominator().to_double()
          == doctest::Approx(std::cbrt(2.0)).epsilon(1e-6));
    CHECK(proxima::cause_of(proxima::realroots(Expr(x) * y - 1).error())
          == proxima::Cause::MaximaError);

    SUBCASE("and found numerically") {
        const auto root = proxima::find_root(proxima::sin(Expr(x)), x, 3.0, 4.0);
        REQUIRE(root.has_value());
        CHECK(*root == doctest::Approx(std::numbers::pi));

        const auto same_sign = proxima::find_root(pow(Expr(x), 2) + 1, x, 0.0, 1.0);
        REQUIRE_FALSE(same_sign.has_value());
        CHECK(same_sign.error().message().find("same sign") != std::string::npos);

        CHECK_FALSE(proxima::find_root(Expr(x) * y, x, -1.0, 1.0).has_value());

        // The interval as written, not to six decimals: to_string made this
        // "between 0.000000 and 0.000000".
        const auto tiny = proxima::find_root(Expr(x) * y, x, 1e-9, 1e-8);
        REQUIRE_FALSE(tiny.has_value());
        CHECK(tiny.error().message().find("between 1e-09 and 1e-08") != std::string::npos);
    }
}

TEST_CASE("ode2 solves an ordinary differential equation, or says it cannot") {
    const Symbol x("x");
    const Symbol y("y");

    const auto growth = proxima::ode2(proxima::eq(proxima::derivative(Expr(y), x), Expr(y)), y, x);
    REQUIRE(growth.has_value());
    REQUIRE(growth->is(Kind::Relation));
    CHECK(proxima::lhs(*growth) == Expr(y));
    CHECK(proxima::rhs(*growth) == Expr::symbol("%c") * proxima::exp(Expr(x)));

    const auto oscillator
        = proxima::ode2(proxima::eq(proxima::derivative(Expr(y), x, 2) + Expr(y), Expr(0)), y, x);
    REQUIRE(oscillator.has_value());
    CHECK(proxima::contains(proxima::rhs(*oscillator), Symbol("%k1")));
    CHECK(proxima::contains(proxima::rhs(*oscillator), Symbol("%k2")));

    SUBCASE("a Failure when it cannot, and the session is none the worse") {
        // ode2 prints its reason to the console before answering false, so
        // that text reaches the pipe ahead of the reply.
        const auto nonlinear = proxima::ode2(
            proxima::eq(pow(proxima::derivative(Expr(y), x), 2), proxima::sin(Expr(y)) * Expr(x)), y, x);
        REQUIRE_FALSE(nonlinear.has_value());
        CHECK(*proxima::expand(pow(Expr(x) + 1, 2)) == Expr(1) + 2 * Expr(x) + pow(Expr(x), 2));
    }
}

TEST_CASE("every builder round-trips through Maxima unchanged") {
    // Sent as structure and read back: the names are Maxima's, and so is
    // the shape, or the two would disagree about what was built.
    const Symbol x("x");
    const std::vector<Expr> built{
        proxima::sin(x),   proxima::cos(x),   proxima::tan(x),     proxima::asin(x),   proxima::acos(x),
        proxima::atan(x),  proxima::sinh(x),  proxima::cosh(x),    proxima::tanh(x),   proxima::asinh(x),
        proxima::acosh(x), proxima::atanh(x), proxima::log(x),     proxima::abs(x),    proxima::erf(x),
        proxima::floor(x), proxima::ceiling(x), proxima::signum(x), proxima::exp(x),   proxima::sqrt(x),
    };
    for (const Expr &expression : built) {
        CAPTURE(expression.str());
        const auto back = proxima::shared_kernel().eval_pure(expression).and_then(proxima::to_expr);
        REQUIRE(back.has_value());
        CHECK(*back == expression);
    }
}

} // TEST_SUITE("maxima")
