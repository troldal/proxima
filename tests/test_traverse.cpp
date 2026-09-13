// Local operations on the expression tree: replace, and contains beside it.
// No kernel anywhere in this file.

#include <doctest/doctest.h>

#include <mx/errors.hpp>
#include <mx/expr.hpp>
#include <mx/functions.hpp>
#include <mx/numeric.hpp>
#include <mx/symbol.hpp>
#include <mx/traverse.hpp>

using mx::Expr;
using mx::Symbol;

TEST_CASE("replace rewrites a symbol locally, and the result is normalised") {
    const Symbol x("x");
    const Symbol y("y");

    // Numbers fold and identities go, as for any freshly built expression.
    CHECK(mx::replace(3 * Expr(x) + 2, x, Expr(2)) == Expr(8));
    CHECK(mx::replace(Expr(x) * Expr(y), y, Expr(1)) == Expr(x));

    // A symbol can stand for an expression, and relations are rewritten too.
    CHECK(mx::replace(Expr(x) * Expr(y), x, Expr(y) + 1) == (Expr(y) + 1) * Expr(y));
    CHECK(mx::replace(eq(Expr(x), Expr(y)), x, Expr(2)) == eq(Expr(2), Expr(y)));

    // Nothing to replace leaves the expression as it was.
    CHECK(mx::replace(Expr(y) + 1, x, Expr(2)) == Expr(y) + 1);

    SUBCASE("but not evaluated, which is Maxima's work") {
        CHECK(mx::replace(mx::sin(Expr(x)), x, Expr(0)) == mx::sin(Expr(0)));
        CHECK(mx::replace(pow(Expr(x), 2), x, Expr(2)) == pow(Expr(2), Expr(2)));
    }

    SUBCASE("a function head is a name, not the symbol") {
        CHECK(mx::replace(Expr::function("x", {Expr(x)}), x, Expr(1))
              == Expr::function("x", {Expr(1)}));
    }

    SUBCASE("unmodelled text that mentions the symbol is refused, not skipped") {
        CHECK_THROWS_AS(mx::replace(Expr::opaque("matrix([x])") + Expr(x), x, Expr(1)),
                        mx::Error);
        CHECK(mx::replace(Expr::opaque("matrix([y])") + Expr(x), x, Expr(1))
              == Expr::opaque("matrix([y])") + 1);
    }

    SUBCASE("and it composes with the numeric layer") {
        // Fix a parameter locally, then compile in the variable.
        const Symbol a("a");
        const Expr f = Expr(a) * pow(Expr(x), 2) + Expr(x);
        const mx::Compiled fixed(mx::replace(f, a, Expr(2.5)), x);
        CHECK(fixed(3.0) == doctest::Approx(mx::evalNumeric(f, {{"a", 2.5}, {"x", 3.0}})));
    }
}

TEST_CASE("contains is reachable from its own header") {
    // It moved here from mx/ops.hpp, which still includes this header.
    const Symbol x("x");
    CHECK(mx::contains(mx::sin(Expr(x)) + 1, x));
    CHECK_FALSE(mx::contains(mx::sin(Expr(Symbol("y"))), x));
}
