// Local operations on the expression tree: replace, and contains beside it.
// No kernel anywhere in this file.

#include <doctest/doctest.h>

#include <cmath>
#include <vector>

#include <proxima/errors.hpp>
#include <proxima/expr.hpp>
#include <proxima/functions.hpp>
#include <proxima/numeric.hpp>
#include <proxima/symbol.hpp>
#include <proxima/traverse.hpp>

using proxima::Expr;
using proxima::Symbol;

TEST_CASE("replace rewrites a symbol locally, and the result is normalised") {
    const Symbol x("x");
    const Symbol y("y");

    // Numbers fold and identities go, as for any freshly built expression.
    CHECK(proxima::replace(3 * Expr(x) + 2, x, Expr(2)) == Expr(8));
    CHECK(proxima::replace(Expr(x) * Expr(y), y, Expr(1)) == Expr(x));

    // A symbol can stand for an expression, and relations are rewritten too.
    CHECK(proxima::replace(Expr(x) * Expr(y), x, Expr(y) + 1) == (Expr(y) + 1) * Expr(y));
    CHECK(proxima::replace(eq(Expr(x), Expr(y)), x, Expr(2)) == eq(Expr(2), Expr(y)));

    // Nothing to replace leaves the expression as it was.
    CHECK(proxima::replace(Expr(y) + 1, x, Expr(2)) == Expr(y) + 1);

    SUBCASE("but not evaluated, which is Maxima's work") {
        CHECK(proxima::replace(proxima::sin(Expr(x)), x, Expr(0)) == proxima::sin(Expr(0)));
        CHECK(proxima::replace(pow(Expr(x), 2), x, Expr(2)) == pow(Expr(2), Expr(2)));
    }

    SUBCASE("a function head is a name, not the symbol") {
        CHECK(proxima::replace(Expr::function("x", {Expr(x)}), x, Expr(1))
              == Expr::function("x", {Expr(1)}));
    }

    SUBCASE("unmodelled text that mentions the symbol is refused, not skipped") {
        CHECK_THROWS_AS(proxima::replace(Expr::opaque("matrix([x])") + Expr(x), x, Expr(1)),
                        proxima::Error);
        CHECK(proxima::replace(Expr::opaque("matrix([y])") + Expr(x), x, Expr(1))
              == Expr::opaque("matrix([y])") + 1);
    }

    SUBCASE("and it composes with the numeric layer") {
        // Fix a parameter locally, then compile in the variable.
        const Symbol a("a");
        const Expr f = Expr(a) * pow(Expr(x), 2) + Expr(x);
        const proxima::Compiled fixed(proxima::replace(f, a, Expr(2.5)), x);
        CHECK(fixed(3.0) == doctest::Approx(proxima::evalNumeric(f, {{"a", 2.5}, {"x", 3.0}})));
    }
}

TEST_CASE("visit goes through every node, a node before its operands") {
    const Symbol x("x");
    // Normalised: the number sorts first, so this is the sum of 1 and sin(x).
    const Expr e = proxima::sin(Expr(x)) + 1;
    std::vector<proxima::Kind> seen;
    proxima::visit(e, [&seen](const Expr &node) { seen.push_back(node.kind()); });
    CHECK(seen == std::vector{proxima::Kind::Add, proxima::Kind::Integer, proxima::Kind::Function,
                              proxima::Kind::Symbol});
}

TEST_CASE("anyOf stops at the first node that answers yes") {
    const Expr e = proxima::sin(Expr(Symbol("x"))) + 1;
    int asked = 0;
    CHECK(proxima::anyOf(e, [&asked](const Expr &node) {
        ++asked;
        return node.is(proxima::Kind::Integer);
    }));
    CHECK(asked == 2); // The sum, then its first operand.

    CHECK_FALSE(proxima::anyOf(e, [](const Expr &node) { return node.is(proxima::Kind::Real); }));
}

TEST_CASE("transform rewrites bottom-up and shares what it leaves alone") {
    const Symbol x("x");
    const Symbol y("y");

    SUBCASE("each node once, operands first, parents rebuilt and normalised") {
        const Expr e = 2 * Expr(x) + 3;
        int calls = 0;
        const Expr doubled = proxima::transform(e, [&calls](const Expr &node) -> Expr {
            ++calls;
            return node.is(proxima::Kind::Integer) ? Expr(node.integerValue()) * 2 : node;
        });
        CHECK(doubled == 4 * Expr(x) + 6);
        CHECK(calls == 5); // 3, then 2 and x, then 2*x, then the sum.
    }

    SUBCASE("an untouched subtree is the same representation, not a copy") {
        const Expr untouched = proxima::sin(Expr(y));
        const Expr e = Expr::function("f", {untouched, Expr(x)});
        const Expr result = proxima::replace(e, x, Expr(1));
        CHECK(proxima::detail::sameRepresentation(result.arg(0), untouched));
        CHECK(proxima::detail::sameRepresentation(proxima::replace(e, Symbol("z"), Expr(1)), e));
    }

    SUBCASE("a rewrite to an equal but different value is kept") {
        // 0.0 == -0.0, so a change between them is visible only by identity.
        const Expr e = Expr::function("f", {Expr(-0.0)});
        const Expr result = proxima::transform(e, [](const Expr &node) -> Expr {
            return node.is(proxima::Kind::Real) ? Expr(0.0) : node;
        });
        CHECK_FALSE(std::signbit(result.arg(0).realValue()));
    }

    SUBCASE("and nothing is evaluated") {
        const Expr result = proxima::transform(proxima::sin(Expr(x)), [&x](const Expr &node) -> Expr {
            return node == Expr(x) ? Expr(0) : node;
        });
        CHECK(result == proxima::sin(Expr(0)));
    }
}

TEST_CASE("contains is reachable from its own header") {
    // It moved here from proxima/ops.hpp, which still includes this header.
    const Symbol x("x");
    CHECK(proxima::contains(proxima::sin(Expr(x)) + 1, x));
    CHECK_FALSE(proxima::contains(proxima::sin(Expr(Symbol("y"))), x));
}
