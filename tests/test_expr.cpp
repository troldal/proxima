// Expr value-type tests. No Maxima, no child process — this is pure C++.

#include <doctest/doctest.h>

#include <mx/errors.hpp>
#include <mx/expr.hpp>
#include <mx/symbol.hpp>

#include <algorithm>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using mx::Expr;
using mx::Kind;
using mx::Symbol;

TEST_CASE("numeric leaves") {
    SUBCASE("integers") {
        CHECK(Expr(42).kind() == Kind::Integer);
        CHECK(Expr(42).integerValue() == 42);
        CHECK(Expr(-7).integerValue() == -7);
        CHECK(Expr().integerValue() == 0);
    }
    SUBCASE("reals are distinct from integers") {
        // 2 and 2.0 are different values to a CAS: one is exact.
        CHECK(Expr(2).kind() == Kind::Integer);
        CHECK(Expr(2.0).kind() == Kind::Real);
        CHECK_FALSE(Expr(2) == Expr(2.0));
    }
    SUBCASE("rationals are reduced on construction") {
        const Expr half = Expr::rational(2, 4);
        CHECK(half.kind() == Kind::Rational);
        CHECK(half.numerator() == 1);
        CHECK(half.denominator() == 2);
    }
    SUBCASE("a whole rational collapses to an integer") {
        CHECK(Expr::rational(6, 3).kind() == Kind::Integer);
        CHECK(Expr::rational(6, 3).integerValue() == 2);
    }
    SUBCASE("the sign lives in the numerator") {
        const Expr value = Expr::rational(1, -2);
        CHECK(value.numerator() == -1);
        CHECK(value.denominator() == 2);
        // So two spellings of the same fraction are the same value.
        CHECK(Expr::rational(1, -2) == Expr::rational(-1, 2));
        CHECK(Expr::rational(-2, -4) == Expr::rational(1, 2));
    }
    SUBCASE("a zero denominator is rejected") {
        CHECK_THROWS_AS(Expr::rational(1, 0), mx::Error);
    }
}

TEST_CASE("integers too large for mx::Integer fall back to Opaque") {
    // The decision this step makes concrete: no multiprecision dependency, and
    // an oversized value is kept exactly as text rather than wrapped. 30! comes
    // up in ordinary use, so this path is real.
    const Expr bignum = Expr::opaque("265252859812191058636308480000000");
    CHECK(bignum.kind() == Kind::Opaque);
    CHECK(bignum.str() == "265252859812191058636308480000000");
    // It is still a value: comparable, hashable, printable.
    CHECK(bignum == Expr::opaque("265252859812191058636308480000000"));
}

TEST_CASE("the one rational that cannot be normalised becomes Opaque") {
    // Negating the extreme negative value would overflow, so rather than wrap
    // silently it takes the same escape hatch bignums use.
    constexpr mx::Integer min = std::numeric_limits<mx::Integer>::min();
    CHECK(Expr::rational(min, -1).kind() == Kind::Opaque);
}

TEST_CASE("symbols") {
    const Symbol x("x");
    CHECK(x.name() == "x");
    CHECK(Expr(x).kind() == Kind::Symbol);
    CHECK(Expr(x) == Expr::symbol("x"));
    CHECK_FALSE(Expr::symbol("x") == Expr::symbol("y"));
}

TEST_CASE("a symbol converts implicitly, so expressions read naturally") {
    using namespace mx::literals;
    const Symbol x("x");

    const Expr f = x * x + 3 * x + 2;
    CHECK(f.kind() == Kind::Add);

    // The literal is equivalent.
    CHECK(Expr("x"_sym) == Expr(x));
}

TEST_CASE("operators build the expected shapes") {
    const Symbol x("x");

    CHECK((Expr(x) + 1).kind() == Kind::Add);
    CHECK((Expr(x) * 2).kind() == Kind::Mul);
    CHECK(pow(Expr(x), 2).kind() == Kind::Pow);

    SUBCASE("subtraction is addition of a negation") {
        const Expr difference = Expr(x) - 1;
        REQUIRE(difference.kind() == Kind::Add);
        // Canonical order puts the constant first, whichever way it was written.
        CHECK(difference.arg(0) == Expr(-1));
        CHECK(difference.arg(1) == Expr(x));
    }
    SUBCASE("dividing exact integers stays exact") {
        // Not 1*3^-1, which would be correct but unhelpful to hand a user.
        const Expr third = Expr(1) / Expr(3);
        CHECK(third.kind() == Kind::Rational);
        CHECK(third.numerator() == 1);
        CHECK(third.denominator() == 3);
    }
    SUBCASE("dividing anything else is multiplication by a reciprocal") {
        // Which is how Maxima represents it internally too. The 1 is the
        // multiplicative identity and the normaliser drops it, leaving the
        // power alone.
        const Expr reciprocal = Expr(1) / Expr(x);
        REQUIRE(reciprocal.kind() == Kind::Pow);
        CHECK(reciprocal.arg(0) == Expr(x));
        CHECK(reciprocal.arg(1) == Expr(-1));
    }
    SUBCASE("negating a literal gives a literal") {
        CHECK((-Expr(5)).kind() == Kind::Integer);
        CHECK((-Expr(5)).integerValue() == -5);
        CHECK((-Expr::rational(1, 2)) == Expr::rational(-1, 2));
        CHECK((-Expr(2.5)).realValue() == doctest::Approx(-2.5));
    }
    SUBCASE("negating anything else multiplies by -1") {
        CHECK((-Expr(x)).kind() == Kind::Mul);
    }
}

TEST_CASE("a sum or product of one is that operand") {
    const Symbol x("x");
    CHECK(Expr::add({Expr(x)}) == Expr(x));
    CHECK(Expr::mul({Expr(x)}) == Expr(x));
    CHECK(Expr::add({}) == Expr(0));
    CHECK(Expr::mul({}) == Expr(1));
}

TEST_CASE("uninterpreted applications carry anything Maxima knows") {
    // The escape hatch that keeps the typed node set small.
    const Symbol x("x");
    const Expr bessel = Expr::function("bessel_j", {Expr(0), Expr(x)});

    CHECK(bessel.kind() == Kind::Function);
    CHECK(bessel.name() == "bessel_j");
    CHECK(bessel.arity() == 2);
    CHECK(bessel.str() == "bessel_j(0, x)");

    // A matrix or a derivative needs no new node type either. Note that
    // "list" is the one head the printer knows by name, because Maxima has no
    // textual list(...) constructor — [a, b] is the only spelling.
    const Expr matrix = Expr::function(
        "matrix", {Expr::function("list", {Expr(1), Expr(2)}),
                   Expr::function("list", {Expr(3), Expr(4)})});
    CHECK(matrix.str() == "matrix([1, 2], [3, 4])");
}

TEST_CASE("relations are built by name, not by operator") {
    const Symbol x("x");
    const Expr equation = eq(Expr(x), Expr(1));

    CHECK(equation.kind() == Kind::Relation);
    CHECK(equation.relationOp() == mx::RelOp::Equal);
    CHECK(equation.str() == "x = 1");
    CHECK(gt(Expr(x), Expr(0)).str() == "x > 0");
    // Maxima spells inequality '#', not '!='.
    CHECK(ne(Expr(x), Expr(0)).str() == "x # 0");
}

TEST_CASE("operator== is structural equality returning bool") {
    // Deliberately not an equation builder. An == that returns something else
    // breaks std::find, unordered containers and every test assertion; SymPy
    // made the same call, and for the same reason.
    const Symbol x("x");

    static_assert(std::is_same_v<decltype(Expr(x) == Expr(x)), bool>);

    CHECK(Expr(x) + 1 == Expr(x) + 1);
    // Structural, so nothing is simplified and nothing consults Maxima.
    CHECK_FALSE(pow(Expr(x) + 1, 2) == Expr(x) * Expr(x) + 2 * Expr(x) + 1);
    // Operand order does not, because the normaliser puts every sum and
    // product into canonical order at construction.
    CHECK(Expr(x) + 1 == Expr(1) + Expr(x));

    SUBCASE("so expressions work in standard containers") {
        std::unordered_set<Expr> seen;
        seen.insert(Expr(x) + 1);
        seen.insert(Expr(x) + 1);
        CHECK(seen.size() == 1);

        std::unordered_map<Expr, int> counts;
        counts[Expr(x)] = 3;
        CHECK(counts.at(Expr::symbol("x")) == 3);

        const std::vector<Expr> terms{Expr(1), Expr(x), Expr(2)};
        CHECK(std::find(terms.begin(), terms.end(), Expr(x)) != terms.end());
    }
}

TEST_CASE("equal expressions hash equally") {
    const Symbol x("x");
    CHECK((Expr(x) + 1).hash() == (Expr(x) + 1).hash());
    CHECK(Expr::rational(2, 4).hash() == Expr::rational(1, 2).hash());
    CHECK(Expr(2).hash() == Expr::rational(4, 2).hash());
}

TEST_CASE("printing parenthesises by precedence") {
    const Symbol x("x");
    const Symbol y("y");

    // Note the canonical order: numbers lead, as they do in Maxima's own
    // internal representation.
    CHECK((Expr(x) + 1).str() == "1 + x");
    CHECK((Expr(x) * 2).str() == "2*x");
    CHECK(pow(Expr(x), 2).str() == "x^2");

    SUBCASE("a sum inside a product is wrapped") {
        CHECK(((Expr(x) + 1) * Expr(y)).str() == "y*(1 + x)");
    }
    SUBCASE("a sum or product inside a power is wrapped") {
        CHECK(pow(Expr(x) + 1, 2).str() == "(1 + x)^2");
        CHECK(pow(Expr(x) * Expr(y), 2).str() == "(x*y)^2");
    }
    SUBCASE("a negative literal base is wrapped, since -3^2 is not (-3)^2") {
        CHECK(pow(Expr(-3), 2).str() == "(-3)^2");
    }
    SUBCASE("a power inside a product is not wrapped") {
        CHECK((pow(Expr(x), 2) * Expr(y)).str() == "y*x^2");
    }
    SUBCASE("a power's own base is wrapped, since ^ is right-associative") {
        CHECK(pow(pow(Expr(x), 2), 3).str() == "(x^2)^3");
    }
}

TEST_CASE("printing renders subtraction rather than adding a negative") {
    const Symbol x("x");

    // A negative leading constant moves to the end for display, so this reads
    // as written rather than as "-1 + x".
    CHECK((Expr(x) - 1).str() == "x - 1");
    CHECK((Expr(x) - 3 * Expr(x)).str() == "x - 3*x");
    // -1*x is a negation, and reads as one.
    CHECK((Expr(x) + -Expr(x)).str() == "x - x");
    // A positive leading constant stays put, since this reads better than
    // "-x + 1".
    CHECK((-Expr(x) + 1).str() == "1 - x");
    // Canonical order puts the number first, where it needs no parentheses:
    // Maxima reads -2*x as -(2*x), the same value.
    CHECK(Expr::mul({Expr(x), Expr(-2)}).str() == "-2*x");
}

TEST_CASE("rationals and reals print recognisably") {
    CHECK(Expr::rational(11, 15).str() == "11/15");
    CHECK(Expr::rational(-1, 2).str() == "-1/2");
    CHECK(Expr(1.5).str() == "1.5");
    // An inexact whole number must not read back as an exact one.
    CHECK(Expr(2.0).str() == "2.0");
}

TEST_CASE("accessors reject the wrong kind instead of returning nonsense") {
    const Symbol x("x");
    CHECK_THROWS_AS(Expr(x).integerValue(), mx::Error);
    CHECK_THROWS_AS(Expr(1).realValue(), mx::Error);
    CHECK_THROWS_AS(Expr(1).name(), mx::Error);
    CHECK_THROWS_AS(Expr(1).relationOp(), mx::Error);
    CHECK_THROWS_AS(Expr(1).opaqueText(), mx::Error);
    CHECK_THROWS_AS(Expr(1).arg(0), mx::Error);
}

TEST_CASE("an integer is a rational with denominator one") {
    // Convenient for arithmetic that does not want to branch on kind.
    CHECK(Expr(5).numerator() == 5);
    CHECK(Expr(5).denominator() == 1);
}

TEST_CASE("copies share representation without sharing identity") {
    const Symbol x("x");
    const Expr original = pow(Expr(x) + 1, 2);
    const Expr copy = original;

    CHECK(copy == original);
    CHECK(copy.hash() == original.hash());
    // Immutable, so sharing is invisible; this is just a cheap copy.
    CHECK(copy.str() == original.str());
}
