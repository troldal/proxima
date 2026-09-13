// Local operations on the expression tree: replace, and contains beside it.
// No kernel anywhere in this file.

#include <doctest/doctest.h>

#include <cmath>
#include <vector>

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

TEST_CASE("visit goes through every node, a node before its operands") {
    const Symbol x("x");
    // Normalised: the number sorts first, so this is the sum of 1 and sin(x).
    const Expr e = mx::sin(Expr(x)) + 1;
    std::vector<mx::Kind> seen;
    mx::visit(e, [&seen](const Expr &node) { seen.push_back(node.kind()); });
    CHECK(seen == std::vector{mx::Kind::Add, mx::Kind::Integer, mx::Kind::Function,
                              mx::Kind::Symbol});
}

TEST_CASE("anyOf stops at the first node that answers yes") {
    const Expr e = mx::sin(Expr(Symbol("x"))) + 1;
    int asked = 0;
    CHECK(mx::anyOf(e, [&asked](const Expr &node) {
        ++asked;
        return node.is(mx::Kind::Integer);
    }));
    CHECK(asked == 2); // The sum, then its first operand.

    CHECK_FALSE(mx::anyOf(e, [](const Expr &node) { return node.is(mx::Kind::Real); }));
}

TEST_CASE("transform rewrites bottom-up and shares what it leaves alone") {
    const Symbol x("x");
    const Symbol y("y");

    SUBCASE("each node once, operands first, parents rebuilt and normalised") {
        const Expr e = 2 * Expr(x) + 3;
        int calls = 0;
        const Expr doubled = mx::transform(e, [&calls](const Expr &node) -> Expr {
            ++calls;
            return node.is(mx::Kind::Integer) ? Expr(node.integerValue()) * 2 : node;
        });
        CHECK(doubled == 4 * Expr(x) + 6);
        CHECK(calls == 5); // 3, then 2 and x, then 2*x, then the sum.
    }

    SUBCASE("an untouched subtree is the same representation, not a copy") {
        const Expr untouched = mx::sin(Expr(y));
        const Expr e = Expr::function("f", {untouched, Expr(x)});
        const Expr result = mx::replace(e, x, Expr(1));
        CHECK(mx::detail::sameRepresentation(result.arg(0), untouched));
        CHECK(mx::detail::sameRepresentation(mx::replace(e, Symbol("z"), Expr(1)), e));
    }

    SUBCASE("a rewrite to an equal but different value is kept") {
        // 0.0 == -0.0, so a change between them is visible only by identity.
        const Expr e = Expr::function("f", {Expr(-0.0)});
        const Expr result = mx::transform(e, [](const Expr &node) -> Expr {
            return node.is(mx::Kind::Real) ? Expr(0.0) : node;
        });
        CHECK_FALSE(std::signbit(result.arg(0).realValue()));
    }

    SUBCASE("and nothing is evaluated") {
        const Expr result = mx::transform(mx::sin(Expr(x)), [&x](const Expr &node) -> Expr {
            return node == Expr(x) ? Expr(0) : node;
        });
        CHECK(result == mx::sin(Expr(0)));
    }
}

TEST_CASE("contains is reachable from its own header") {
    // It moved here from mx/ops.hpp, which still includes this header.
    const Symbol x("x");
    CHECK(mx::contains(mx::sin(Expr(x)) + 1, x));
    CHECK_FALSE(mx::contains(mx::sin(Expr(Symbol("y"))), x));
}
