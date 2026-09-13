// The operations. Split in two: the parts that need no kernel are in the
// default unit run, the rest are integration tests against real Maxima.

#include <doctest/doctest.h>

#include <mx/errors.hpp>
#include <mx/expr.hpp>
#include <mx/functions.hpp>
#include <mx/ops.hpp>
#include <mx/symbol.hpp>

#include <span>
#include <string>
#include <vector>

using mx::Expr;
using mx::Kind;
using mx::Symbol;

// --- Maxima-free ----------------------------------------------------------

TEST_CASE("contains finds a symbol anywhere in an expression") {
    const Symbol x("x");
    const Symbol y("y");

    CHECK(mx::contains(Expr(x), x));
    CHECK_FALSE(mx::contains(Expr(y), x));
    CHECK(mx::contains(pow(Expr(x) + 1, 2) * Expr(y), x));
    CHECK(mx::contains(mx::sin(mx::cos(Expr(x))), x));
    CHECK_FALSE(mx::contains(Expr(1) / Expr(3), x));
    // A symbol whose name merely starts the same is a different symbol.
    CHECK_FALSE(mx::contains(Expr::symbol("xy"), x));
}

TEST_CASE("contains looks inside Opaque text as well") {
    // solve rejects an answer that still mentions its unknown — `[x = sin(x)]`
    // is Maxima saying it could not finish. A mention hidden in unmodelled text
    // used to get through that check.
    const Symbol x("x");

    CHECK(mx::contains(Expr::opaque("sin(x) + 1"), x));
    CHECK(mx::contains(Expr::function("f", {Expr::opaque("x^2")}), x));
    CHECK_FALSE(mx::contains(Expr::opaque("sin(y) + 1"), x));

    SUBCASE("as a whole identifier, not part of a longer one") {
        CHECK(mx::contains(Expr::opaque("2*x+1"), x));
        CHECK_FALSE(mx::contains(Expr::opaque("xy + x_1 + %x + x2"), x));
    }

    SUBCASE("and not inside a string literal") {
        CHECK_FALSE(mx::contains(Expr::opaque(R"("x marks the spot")"), x));
        CHECK_FALSE(mx::contains(Expr::opaque(R"("say \"x\" twice")"), x));
        CHECK(mx::contains(Expr::opaque(R"(concat("a", x))"), x));
    }

    SUBCASE("while a name that is not a plain identifier is found as written") {
        // Maxima source spells the symbol `x y` with a backslash.
        const Symbol spaced("x y");
        CHECK(mx::contains(Expr::opaque(R"(f(x\ y))"), spaced));
        CHECK_FALSE(mx::contains(Expr::opaque("f(x, y)"), spaced));
    }
}

TEST_CASE("function builders produce uninterpreted applications") {
    const Symbol x("x");

    CHECK(mx::sin(Expr(x)).str() == "sin(x)");
    CHECK(mx::log(Expr(x)).str() == "log(x)");
    // Nothing is evaluated locally; sin(0) stays sin(0) until Maxima is asked.
    CHECK(mx::sin(Expr(0)).kind() == Kind::Function);

    SUBCASE("except the two Maxima has no node for either") {
        // exp is %e^x and sqrt is x^(1/2), internally in Maxima as well, so
        // building them that way keeps the representations in step.
        CHECK(mx::exp(Expr(x)).kind() == Kind::Pow);
        CHECK(mx::exp(Expr(x)).str() == "%e^x");
        CHECK(mx::sqrt(Expr(x)).kind() == Kind::Pow);
        // Parenthesised, because x^1/2 would parse as (x^1)/2.
        CHECK(mx::sqrt(Expr(x)).str() == "x^(1/2)");
    }
}

TEST_CASE("constants are spelled as Maxima names them") {
    CHECK(mx::pi().str() == "%pi");
    CHECK(mx::e().str() == "%e");
    CHECK(mx::inf().str() == "inf");
}

// --- Against a real kernel -------------------------------------------------

TEST_SUITE("maxima") {

TEST_CASE("the shared kernel is one process, reused") {
    CHECK(&mx::sharedKernel() == &mx::sharedKernel());

    // A binding made by one call is visible to the next, which is the
    // observable consequence of there being a single long-lived process.
    // Note this goes through Kernel::eval, not mx::parse: parsing is only
    // parsing, and deliberately does not evaluate what it reads.
    REQUIRE(mx::sharedKernel().eval("shared_probe: 11").ok);
    CHECK(mx::sharedKernel().eval("shared_probe^2").value == "121");
}

TEST_CASE("differentiation") {
    const Symbol x("x");
    CHECK(mx::diff(pow(Expr(x), 2), x) == Expr(2) * Expr(x));
    CHECK(mx::diff(mx::sin(Expr(x)), x) == mx::cos(Expr(x)));

    SUBCASE("to higher order") {
        CHECK(mx::diff(mx::sin(Expr(x)), x, 2) == -mx::sin(Expr(x)));
        CHECK(mx::diff(pow(Expr(x), 3), x, 3) == Expr(6));
    }
    SUBCASE("with respect to an absent symbol is zero") {
        CHECK(mx::diff(Expr::symbol("y"), x) == Expr(0));
    }
}

TEST_CASE("algebraic rearrangement") {
    const Symbol x("x");

    CHECK(mx::expand(pow(Expr(x) + 1, 2))
          == Expr(1) + 2 * Expr(x) + pow(Expr(x), 2));
    CHECK(mx::simplify((pow(Expr(x), 2) - 1) / (Expr(x) - 1)) == Expr(x) + 1);
    CHECK(mx::subst(pow(Expr(x), 2) + 1, x, Expr(5)) == Expr(26));

    SUBCASE("factoring returns a product") {
        const Expr factored = mx::factor(pow(Expr(x), 2) - 1);
        CHECK(factored.kind() == Kind::Mul);
        // And expanding it again recovers the original.
        CHECK(mx::expand(factored) == pow(Expr(x), 2) - 1);
    }
}

TEST_CASE("integration") {
    const Symbol x("x");

    const auto integral = mx::integrate(pow(Expr(x), 2), x);
    REQUIRE(integral.has_value());
    CHECK(*integral == pow(Expr(x), 3) / Expr(3));

    SUBCASE("a definite integral evaluates exactly") {
        const auto area = mx::integrate(pow(Expr(x), 2), x, Expr(0), Expr(1));
        REQUIRE(area.has_value());
        CHECK(*area == Expr::rational(1, 3));
    }

    SUBCASE("differentiating the result recovers the integrand") {
        const auto antiderivative
            = mx::integrate(pow(Expr(x), 2) * mx::sin(Expr(x)), x);
        REQUIRE(antiderivative.has_value());
        CHECK(mx::simplify(mx::diff(*antiderivative, x))
              == pow(Expr(x), 2) * mx::sin(Expr(x)));
    }

    SUBCASE("no closed form is a Failure, not an exception") {
        // Maxima signals this by returning the integral unevaluated rather
        // than by erroring, so the noun form is what gets recognised.
        const auto hopeless = mx::integrate(mx::exp(mx::sin(Expr(x))), x);
        REQUIRE_FALSE(hopeless.has_value());
        CHECK(hopeless.error().message.find("no closed form")
              != std::string::npos);
    }

    SUBCASE("a genuine Maxima error is also a Failure") {
        // A divergent definite integral is something Maxima *errors* on,
        // rather than handing back unevaluated.
        const auto bad = mx::integrate(Expr(1) / Expr(x), x, Expr(0), Expr(1));
        REQUIRE_FALSE(bad.has_value());
        CHECK(bad.error().message.find("divergent") != std::string::npos);
    }
}

TEST_CASE("limits") {
    const Symbol x("x");

    const auto sinc = mx::limit(mx::sin(Expr(x)) / Expr(x), x, Expr(0));
    REQUIRE(sinc.has_value());
    CHECK(*sinc == Expr(1));

    SUBCASE("one-sided limits differ from the two-sided one") {
        const auto above
            = mx::limit(Expr(1) / Expr(x), x, Expr(0), mx::Side::FromAbove);
        const auto below
            = mx::limit(Expr(1) / Expr(x), x, Expr(0), mx::Side::FromBelow);
        REQUIRE(above.has_value());
        REQUIRE(below.has_value());
        CHECK(above->str() == "inf");
        CHECK(below->str() == "minf");
    }

    SUBCASE("at infinity") {
        const auto decay = mx::limit(Expr(1) / Expr(x), x, mx::inf());
        REQUIRE(decay.has_value());
        CHECK(*decay == Expr(0));
    }
}

TEST_CASE("solving") {
    const Symbol x("x");

    const auto roots = mx::solve(eq(pow(Expr(x), 2), Expr(1)), x);
    REQUIRE(roots.has_value());
    REQUIRE(roots->size() == 2);
    CHECK((*roots)[0] == Expr(-1));
    CHECK((*roots)[1] == Expr(1));

    SUBCASE("a linear equation has one solution") {
        const auto one = mx::solve(eq(2 * Expr(x) + 1, Expr(0)), x);
        REQUIRE(one.has_value());
        REQUIRE(one->size() == 1);
        CHECK((*one)[0] == Expr::rational(-1, 2));
    }

    SUBCASE("an equation Maxima cannot rearrange is a Failure") {
        // Maxima returns [x = sin(x)] here: an equation, but not a solution.
        // Accepting it would hand the caller something useless that looks like
        // an answer.
        const auto stuck = mx::solve(eq(mx::sin(Expr(x)), Expr(x)), x);
        REQUIRE_FALSE(stuck.has_value());
        CHECK(stuck.error().message.find("did not solve") != std::string::npos);
    }

    SUBCASE("and so is one it never rearranges at all") {
        // [0 = x^5 - x - 1]: the unknown is not even on the left.
        const auto quintic
            = mx::solve(eq(pow(Expr(x), 5) - Expr(x) - 1, Expr(0)), x);
        CHECK_FALSE(quintic.has_value());
    }

    SUBCASE("no solutions is an answer, not a failure") {
        const auto none = mx::solve(eq(Expr(1), Expr(0)), x);
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

    const auto solutions = mx::solve(linear, unknowns);
    REQUIRE(solutions.has_value());
    REQUIRE(solutions->size() == 1);
    REQUIRE(solutions->front().size() == 2);
    CHECK(solutions->front()[0] == Expr(2)); // x
    CHECK(solutions->front()[1] == Expr(1)); // y

    SUBCASE("values follow the order the unknowns were asked for") {
        // Maxima answers in whatever order it likes; the correspondence is
        // established by name, not by position.
        const std::vector<Symbol> reversed{y, x};
        const auto swapped = mx::solve(linear, reversed);
        REQUIRE(swapped.has_value());
        REQUIRE(swapped->front().size() == 2);
        CHECK(swapped->front()[0] == Expr(1)); // y
        CHECK(swapped->front()[1] == Expr(2)); // x
    }

    SUBCASE("a system with several solutions returns each of them") {
        const std::vector<Expr> circle{
            eq(pow(Expr(x), 2) + pow(Expr(y), 2), Expr(1)),
            eq(Expr(y), Expr(x))};
        const auto both = mx::solve(circle, unknowns);
        REQUIRE(both.has_value());
        CHECK(both->size() == 2);
        // On this circle the two coordinates are equal in both solutions.
        for (const mx::Solution &solution : *both) {
            REQUIRE(solution.size() == 2);
            CHECK(solution[0] == solution[1]);
        }
    }

    SUBCASE("no solution is an answer, not a failure") {
        const std::vector<Expr> contradictory{eq(Expr(x), Expr(1)),
                                              eq(Expr(x), Expr(2))};
        const std::vector<Symbol> justX{x};
        const auto none = mx::solve(contradictory, justX);
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

    const auto solutions = mx::solve(equations, unknowns);
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

    const auto solutions = mx::solve(equations, unknowns);
    REQUIRE(solutions.has_value());
    REQUIRE(solutions->size() == 1);

    // y is the parameter and x is 3 minus it, so the two still sum to 3.
    const mx::Solution &solution = solutions->front();
    REQUIRE(solution.size() == 2);
    CHECK(mx::simplify(solution[0] + solution[1]) == Expr(3));
}

TEST_CASE("a system Maxima cannot solve is a Failure") {
    const Symbol x("x");
    const Symbol y("y");
    const std::vector<Expr> equations{eq(mx::sin(Expr(x)), Expr(x)),
                                      eq(Expr(y), Expr(1))};
    const std::vector<Symbol> unknowns{x, y};

    CHECK_FALSE(mx::solve(equations, unknowns).has_value());
}

TEST_CASE("solving a system rejects degenerate arguments") {
    const Symbol x("x");
    const std::vector<Expr> equations{eq(Expr(x), Expr(1))};
    const std::vector<Symbol> unknowns{x};

    CHECK_FALSE(mx::solve(equations, std::span<const Symbol>{}).has_value());
    CHECK_FALSE(mx::solve(std::span<const Expr>{}, unknowns).has_value());
}

TEST_CASE("parsing delegates to Maxima's own parser") {
    const auto parsed = mx::parse("x^2 + 3*x + 2");
    REQUIRE(parsed.has_value());
    CHECK(*parsed == pow(Expr::symbol("x"), 2) + 3 * Expr::symbol("x") + 2);

    SUBCASE("exactness survives") {
        const auto third = mx::parse("1/3");
        REQUIRE(third.has_value());
        CHECK(*third == Expr::rational(1, 3));
    }
    SUBCASE("malformed input is a Failure, not an exception") {
        const auto broken = mx::parse("this is not maxima ][");
        CHECK_FALSE(broken.has_value());
    }
    SUBCASE("a quote in the source does not break the call") {
        // The source is embedded in a Maxima string literal, so it has to be
        // escaped on the way in.
        const auto quoted = mx::parse("\"a string\"");
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
    CHECK_THROWS_AS(mx::diff(Expr::opaque("(1"), Symbol("x")), mx::MaximaError);
}

TEST_CASE("results are canonical expressions, not text") {
    // The whole point of the layer: what comes back is an Expr that can be fed
    // straight into the next operation.
    const Symbol x("x");
    const Expr chained
        = mx::expand(mx::factor(mx::diff(pow(Expr(x), 3) + pow(Expr(x), 2), x)));
    CHECK(chained == 3 * pow(Expr(x), 2) + 2 * Expr(x));
}

} // TEST_SUITE("maxima")
