// Expr value-type tests. No Maxima, no child process — this is pure C++.

#include <doctest/doctest.h>

#include <proxima/errors.hpp>
#include <proxima/expr.hpp>
#include <proxima/symbol.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using proxima::Expr;
using proxima::Kind;
using proxima::Symbol;

// --- what an expression can be built from ------------------------------------
//
// Checked at compile time, since that is where the trap was. C++ converts bool
// and the character types to numbers implicitly, and Expr used to take them
// that way: Expr(true) was the Real 1.0, Expr('a') the Real 97.0, Expr(u'a')
// the Integer 97, and `x + true` quietly `1.0 + x`.

static_assert(!std::is_constructible_v<Expr, bool>);
static_assert(!std::is_constructible_v<Expr, char>);
static_assert(!std::is_constructible_v<Expr, wchar_t>);
static_assert(!std::is_constructible_v<Expr, char8_t>);
static_assert(!std::is_constructible_v<Expr, char16_t>);
static_assert(!std::is_constructible_v<Expr, char32_t>);
// Nor implicitly, which is what an operator's argument needs: `x + true`.
static_assert(!std::is_convertible_v<bool, Expr>);
static_assert(!std::is_convertible_v<char, Expr>);
static_assert(!std::is_convertible_v<char16_t, Expr>);

// Every numeric type still converts, including the fixed-width ones that are
// spelled with signed char and unsigned char.
static_assert(std::is_convertible_v<int, Expr>);
static_assert(std::is_convertible_v<long long, Expr>);
static_assert(std::is_convertible_v<unsigned, Expr>);
static_assert(std::is_convertible_v<std::size_t, Expr>);
static_assert(std::is_convertible_v<std::int8_t, Expr>);
static_assert(std::is_convertible_v<std::uint8_t, Expr>);
static_assert(std::is_convertible_v<float, Expr>);
static_assert(std::is_convertible_v<double, Expr>);
static_assert(std::is_convertible_v<long double, Expr>);
static_assert(std::is_convertible_v<proxima::Integer, Expr>);

TEST_CASE("small fixed-width integers are numbers, not characters") {
    // std::int8_t is signed char, which is why only plain char is excluded.
    CHECK(Expr(std::int8_t{-3}).kind() == Kind::Integer);
    CHECK(Expr(std::int8_t{-3}).integerValue() == -3);
    CHECK(Expr(std::uint8_t{200}).integerValue() == 200);

    SUBCASE("and every floating-point type is a Real") {
        CHECK(Expr(2.5f).kind() == Kind::Real);
        CHECK(Expr(2.5f).realValue() == 2.5);
        CHECK(Expr(2.5L).kind() == Kind::Real);
    }
}

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
        CHECK_THROWS_AS(Expr::rational(1, 0), proxima::Error);
    }
}

TEST_CASE("large integers are numbers, not text") {
    // proxima::Integer is unbounded, so 30! is an Integer node that arithmetic
    // works on — it used to become Opaque text that could only be printed.
    const Expr bignum = Expr(proxima::Integer("265252859812191058636308480000000"));
    CHECK(bignum.kind() == Kind::Integer);
    CHECK(bignum.str() == "265252859812191058636308480000000");
    CHECK(bignum == Expr(proxima::Integer("265252859812191058636308480000000")));

    SUBCASE("and can be computed with") {
        CHECK((bignum + Expr(1)).str() == "265252859812191058636308480000001");
        CHECK((bignum - bignum) == Expr(0));
    }
}

TEST_CASE("the extreme 64-bit value is no longer a special case") {
    // This used to become Opaque, because negating it overflowed. Nothing
    // overflows now.
    const Expr value = Expr::rational(proxima::Integer("-9223372036854775808"),
                                      proxima::Integer(-1));
    CHECK(value.kind() == Kind::Integer);
    CHECK(value.str() == "9223372036854775808");
}

TEST_CASE("symbols") {
    const Symbol x("x");
    CHECK(x.name() == "x");
    CHECK(Expr(x).kind() == Kind::Symbol);
    CHECK(Expr(x) == Expr::symbol("x"));
    CHECK_FALSE(Expr::symbol("x") == Expr::symbol("y"));
}

TEST_CASE("a symbol converts implicitly, so expressions read naturally") {
    using namespace proxima::literals;
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
    CHECK(equation.relationOp() == proxima::RelOp::Equal);
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

        // Symbols too, hashing as the expression they are.
        const std::unordered_set<Symbol> symbols{x, Symbol("x"), Symbol("y")};
        CHECK(symbols.size() == 2);
        CHECK(std::hash<Symbol>{}(x) == std::hash<Expr>{}(Expr(x)));

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

TEST_CASE("a Real is never NaN or infinite") {
    // A NaN is not equal to itself. Held in an Expr it broke the ordering the
    // normaliser sorts by (undefined behaviour in std::sort), made equality
    // disagree with the hash, and printed as `nan`, a symbol to Maxima.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    CHECK_THROWS_AS(Expr::real(nan), proxima::Error);
    // Braces: `Expr(nan);` as a statement declares a variable named nan.
    CHECK_THROWS_AS(Expr{nan}, proxima::Error);

    SUBCASE("an infinity is Maxima's symbol, which reads back as itself") {
        // Found by fuzzing: a Real infinity printed as inf, which reads back
        // as the symbol, a different expression. Maxima has only the symbol.
        CHECK(Expr(inf) == Expr::symbol("inf"));
        CHECK(Expr(-inf) == Expr::symbol("minf"));
        CHECK(Expr::real(inf).kind() == Kind::Symbol);
        CHECK(Expr::parse(Expr(-inf).str()) == Expr(-inf));

        // A symbol does not fold, so neither sum is refused as NaN any more.
        const Symbol x("x");
        CHECK((Expr(inf) + Expr(1.0)).kind() == Kind::Add);
        CHECK_NOTHROW(static_cast<void>(Expr(inf) + Expr(-inf)));
        CHECK(Expr(x) + Expr(inf) == Expr(inf) + Expr(x));
    }

    SUBCASE("and arithmetic that would overflow to one is refused") {
        // As Maxima refuses it, with FLOATING-POINT-OVERFLOW. It used to fold
        // silently to infinity, from finite numbers.
        CHECK_THROWS_AS(static_cast<void>(Expr(1e308) * Expr(10.0)), proxima::Error);
        CHECK_THROWS_AS(static_cast<void>(Expr(1e308) + Expr(1e308)), proxima::Error);
        const Expr huge(proxima::Integer("1" + std::string(400, '0')));
        CHECK_THROWS_AS(static_cast<void>(huge * Expr(1.0)), proxima::Error);
        CHECK_THROWS_AS(static_cast<void>(Expr::parse("1" + std::string(400, '0') + "/5.0")),
                        proxima::Error);

        // Dividing by a tiny real would overflow its reciprocal: it stays a
        // negative power instead.
        CHECK_NOTHROW(static_cast<void>(Expr(2.0) / Expr(1e-320)));
    }
}

TEST_CASE("negative zero equals zero, so it hashes equally") {
    // MSVC's std::hash<double> hashes the bit pattern, where libstdc++ special-
    // cases zero; an unordered container then failed to find one by the other.
    const Expr zero = Expr::real(0.0);
    const Expr negativeZero = Expr::real(-0.0);
    CHECK(zero == negativeZero);
    CHECK(zero.hash() == negativeZero.hash());

    const std::unordered_set<Expr> set{zero};
    CHECK(set.count(negativeZero) == 1);
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
        // A symbolic exponent: an integer one, (x^2)^3, is combined into x^6.
        CHECK(pow(pow(Expr(x), 2), Expr(y)).str() == "(x^2)^y");
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
    CHECK_THROWS_AS(Expr(x).integerValue(), proxima::Error);
    CHECK_THROWS_AS(Expr(1).realValue(), proxima::Error);
    CHECK_THROWS_AS(Expr(1).name(), proxima::Error);
    CHECK_THROWS_AS(Expr(1).relationOp(), proxima::Error);
    CHECK_THROWS_AS(Expr(1).opaqueText(), proxima::Error);
    CHECK_THROWS_AS(Expr(1).arg(0), proxima::Error);
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
