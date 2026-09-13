// Normalisation tests. No Maxima, no child process.
//
// The normaliser exists to make structurally identical expressions *look*
// identical, so equality and hashing mean something and cache keys hit. It is
// deliberately not algebra — the second half of this file is as important as
// the first.

#include <doctest/doctest.h>

#include <mx/expr.hpp>
#include <mx/symbol.hpp>

#include <cmath>
#include <limits>
#include <string>

using mx::Expr;
using mx::Kind;
using mx::Symbol;

TEST_CASE("nested sums and products are flattened") {
    const Symbol x("x");
    const Symbol y("y");
    const Symbol z("z");

    const Expr sum = (Expr(x) + Expr(y)) + Expr(z);
    REQUIRE(sum.kind() == Kind::Add);
    CHECK(sum.arity() == 3);

    const Expr product = (Expr(x) * Expr(y)) * Expr(z);
    REQUIRE(product.kind() == Kind::Mul);
    CHECK(product.arity() == 3);

    // So associativity holds structurally, which a nested shape would break.
    CHECK((Expr(x) + Expr(y)) + Expr(z) == Expr(x) + (Expr(y) + Expr(z)));
    CHECK((Expr(x) * Expr(y)) * Expr(z) == Expr(x) * (Expr(y) * Expr(z)));
}

TEST_CASE("operand order is canonical, so commutativity holds") {
    const Symbol x("x");
    const Symbol y("y");

    CHECK(Expr(x) + 1 == Expr(1) + Expr(x));
    CHECK(Expr(x) * 2 == Expr(2) * Expr(x));
    CHECK(Expr(x) + Expr(y) == Expr(y) + Expr(x));
    CHECK(Expr(x) * Expr(y) == Expr(y) * Expr(x));

    SUBCASE("and equal expressions hash equally") {
        CHECK((Expr(x) + 1).hash() == (Expr(1) + Expr(x)).hash());
        CHECK((Expr(x) * Expr(y)).hash() == (Expr(y) * Expr(x)).hash());
    }
}

TEST_CASE("numbers are folded into one term") {
    const Symbol x("x");

    CHECK(Expr(1) + Expr(2) == Expr(3));
    CHECK(Expr(2) * Expr(3) == Expr(6));
    CHECK(Expr(1) + Expr(2) + Expr(x) == Expr(3) + Expr(x));
    CHECK((Expr(2) * Expr(3) * Expr(x)).str() == "6*x");

    SUBCASE("exactly, for exact operands") {
        CHECK(Expr::rational(1, 2) + Expr::rational(1, 3)
              == Expr::rational(5, 6));
        CHECK(Expr::rational(2, 3) * Expr::rational(3, 4)
              == Expr::rational(1, 2));
        // A fraction that comes out whole collapses.
        CHECK(Expr::rational(1, 2) + Expr::rational(1, 2) == Expr(1));
    }

    SUBCASE("inexactness is contagious, as it is in Maxima") {
        const Expr mixed = Expr(1) + Expr(2.5);
        REQUIRE(mixed.kind() == Kind::Real);
        CHECK(mixed.realValue() == doctest::Approx(3.5));
    }

    SUBCASE("regardless of the order they were written in") {
        CHECK(Expr(1) + Expr(x) + Expr(2) == Expr(3) + Expr(x));
        CHECK(Expr(2) * Expr(x) * Expr(3) == Expr(6) * Expr(x));
    }
}

TEST_CASE("identities are dropped") {
    const Symbol x("x");

    CHECK(Expr(x) + 0 == Expr(x));
    CHECK(Expr(x) * 1 == Expr(x));
    CHECK(Expr(x) * 0 == Expr(0));

    SUBCASE("but a lone identity survives, since it is the whole value") {
        CHECK(Expr::add({Expr(0), Expr(0)}) == Expr(0));
        CHECK(Expr::mul({Expr(1), Expr(1)}) == Expr(1));
    }

    SUBCASE("and folding can produce one") {
        // 1 + -1 folds to 0, which then vanishes from the sum.
        CHECK(Expr(1) + Expr(x) + Expr(-1) == Expr(x));
        CHECK(Expr(2) * Expr(x) * Expr::rational(1, 2) == Expr(x));
    }
}

TEST_CASE("the identities of exponentiation apply") {
    const Symbol x("x");

    CHECK(pow(Expr(x), 1) == Expr(x));
    CHECK(pow(Expr(x), 0) == Expr(1));
    CHECK(pow(Expr(1), Expr(x)) == Expr(1));

    SUBCASE("0^0 is left for Maxima to have an opinion about") {
        CHECK(pow(Expr(0), 0).kind() == Kind::Pow);
    }
}

TEST_CASE("folding cannot overflow, so it never gives up") {
    // This used to abandon the fold and leave the terms unevaluated, because
    // the sum did not fit in 64 bits. mx::Integer is unbounded now, so the
    // arithmetic simply happens.
    const mx::Integer max("9223372036854775807");

    const Expr sum = Expr(max) + Expr(max);
    REQUIRE(sum.kind() == Kind::Integer);
    CHECK(sum.str() == "18446744073709551614");

    const Expr product = Expr(max) * Expr(max);
    REQUIRE(product.kind() == Kind::Integer);
    CHECK(product.str() == "85070591730234615847396907784232501249");

    SUBCASE("a long sum of fractions stays exact and stays small") {
        // Reducing after every step keeps the denominators from ballooning —
        // which matters more now that nothing stops them.
        Expr total = Expr(0);
        for (int i = 1; i <= 40; ++i) {
            total = total + Expr::rational(mx::Integer(1), mx::Integer(i));
        }
        REQUIRE(total.kind() == Kind::Rational);
        // The 40th harmonic number, exactly, and in lowest terms.
        CHECK(total.numerator().toString() == "2078178381193813");
        CHECK(total.denominator().toString() == "485721041551200");
    }
}

TEST_CASE("canonical order is platform-independent") {
    // Ordering by hash would have been simpler, but std::hash<std::string>
    // differs between standard libraries, which would make canonical form —
    // and therefore printed output and these very expectations — vary by
    // platform. The order is structural instead.
    const Symbol x("x");
    const Symbol y("y");
    const Symbol a("a");

    // Numbers, then symbols alphabetically, then compounds.
    CHECK((Expr(y) + Expr(a) + Expr(2)).str() == "2 + a + y");
    CHECK((Expr(x) * Expr(a) * Expr(3)).str() == "3*a*x");
}

// --- what the normaliser deliberately does not do -------------------------

TEST_CASE("like terms are not collected") {
    // That is algebra, and algebra belongs to Maxima. One canonicaliser is the
    // whole reason SymEngine was dropped; doing half of it here would
    // reintroduce exactly the problem that decision avoided.
    const Symbol x("x");

    CHECK_FALSE(Expr(x) - Expr(x) == Expr(0));
    CHECK_FALSE(Expr(x) + Expr(x) == Expr(2) * Expr(x));
}

TEST_CASE("nothing is expanded or factored") {
    const Symbol x("x");

    CHECK_FALSE(pow(Expr(x) + 1, 2)
                == Expr(x) * Expr(x) + 2 * Expr(x) + 1);
    CHECK_FALSE((Expr(x) + 1) * (Expr(x) - 1) == pow(Expr(x), 2) - 1);
}

TEST_CASE("powers are not combined") {
    const Symbol x("x");
    CHECK_FALSE(pow(Expr(x), 2) * pow(Expr(x), 3) == pow(Expr(x), 5));
}

TEST_CASE("building without extra copies keeps every normalisation rule") {
    // The normaliser now works in the caller's vector and hands back a lone
    // number instead of rebuilding it. These are the rules that could change
    // without anything else in this file noticing.
    const Symbol x("x");
    const Symbol y("y");

    SUBCASE("exact identities go, and an exact zero absorbs a product") {
        CHECK(Expr(x) + 0 == Expr(x));
        CHECK(Expr(0) + Expr(x) == Expr(x));
        CHECK(Expr(x) * 1 == Expr(x));
        CHECK((Expr(x) * 0).kind() == Kind::Integer);
        CHECK(Expr(x) * 0 == Expr(0));
    }

    SUBCASE("inexact ones are not identities") {
        CHECK((Expr(0.0) + Expr(x)).kind() == Kind::Add);
        CHECK((Expr(1.0) * Expr(x)).kind() == Kind::Mul);
        CHECK((Expr(0.0) * Expr(x)).kind() == Kind::Mul);
    }

    SUBCASE("a lone number is kept exactly as it was") {
        const Expr third = Expr::rational(1, 3);
        const Expr sum = third + Expr(x);
        REQUIRE(sum.kind() == Kind::Add);
        CHECK(sum.arg(0) == third);
        CHECK((Expr(2.5) * Expr(x)).arg(0) == Expr(2.5));
    }

    SUBCASE("a lone -0.0 folds to +0.0 in a sum, and keeps its sign in a product") {
        // What the folding arithmetic always did: 0.0 + -0.0 is +0.0, and
        // 1.0 * -0.0 is -0.0. Returning the lone number untouched must not
        // change either.
        const Expr sum = Expr::add({Expr(-0.0), Expr(x)});
        REQUIRE(sum.kind() == Kind::Add);
        CHECK_FALSE(std::signbit(sum.arg(0).realValue()));

        const Expr product = Expr::mul({Expr(-0.0), Expr(x)});
        REQUIRE(product.kind() == Kind::Mul);
        CHECK(std::signbit(product.arg(0).realValue()));
    }

    SUBCASE("reals handed over together fold in canonical order") {
        // Not a chain of binary +: each + folds as it goes, so
        // 0.1 + 0.2 + 0.3 and 0.3 + 0.2 + 0.1 add in different orders and can
        // differ in the last bits, which is floating point, not the
        // normaliser. Numbers given to one sum are sorted before folding, so
        // their order does not matter.
        CHECK(Expr::add({Expr(0.1), Expr(0.2), Expr(0.3), Expr(x)})
              == Expr::add({Expr(0.3), Expr(x), Expr(0.2), Expr(0.1)}));
    }

    SUBCASE("flattening still folds a single number and sorts the rest") {
        const Expr nested = (Expr(x) + 1) + Expr(y);
        REQUIRE(nested.kind() == Kind::Add);
        CHECK(nested.arity() == 3);
        CHECK(nested == Expr(y) + Expr(1) + Expr(x));
        CHECK((Expr(x) + 1) + (Expr(y) + 2) == Expr(x) + Expr(y) + 3);
    }
}

TEST_CASE("exact and inexact stay distinguishable") {
    // 2 and 2.0 must not be folded together, or exactness would be lost the
    // moment a float appeared anywhere in an expression.
    CHECK_FALSE(Expr(2) == Expr(2.0));
    CHECK_FALSE(Expr::rational(1, 2) == Expr(0.5));
}
