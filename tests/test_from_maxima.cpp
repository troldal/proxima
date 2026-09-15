// Maxima -> Expr mapping. Pure: no kernel, no child process.

#include <doctest/doctest.h>

#include "wire/from_maxima.hpp"
#include "wire/sexpr.hpp"

#include <proxima/errors.hpp>
#include <proxima/expr.hpp>

#include <fstream>
#include <sstream>
#include <string>

using proxima::Expr;
using proxima::Kind;
using proxima::detail::decodeMaximaName;
using proxima::detail::fromMaxima;
using proxima::detail::parseSExpr;

namespace {

/// Reads an internal form and renders it back as Maxima infix — the whole
/// inbound path in one step, which is what most of these cases care about.
std::string mapped(const char *internalForm) {
    return fromMaxima(parseSExpr(internalForm)).str();
}

Expr mapExpr(const char *internalForm) {
    return fromMaxima(parseSExpr(internalForm));
}

} // namespace

TEST_CASE("symbol names undo Maxima's case inversion") {
    // Maxima stores x as $X and X as $x, inverting uniformly-cased names and
    // leaving mixed-case ones (which reached it bar-quoted) alone. Getting this
    // wrong would rename every variable in the library.
    CHECK(decodeMaximaName("X") == "x");
    CHECK(decodeMaximaName("x") == "X");
    CHECK(decodeMaximaName("xY") == "xY");
    CHECK(decodeMaximaName("X_1") == "x_1");
    CHECK(decodeMaximaName("ALPHA") == "alpha");
    CHECK(decodeMaximaName("%GAMMA") == "%gamma");
    CHECK(decodeMaximaName("") == "");
    CHECK(decodeMaximaName("_") == "_");

    SUBCASE("decoding is its own inverse") {
        for (const char *name : {"X", "x", "xY", "X_1", "%PI"}) {
            CAPTURE(name);
            CHECK(decodeMaximaName(decodeMaximaName(name)) == std::string(name));
        }
    }
}

TEST_CASE("atoms") {
    CHECK(mapped("42") == "42");
    CHECK(mapped("-7") == "-7");
    CHECK(mapped("$X") == "x");
    CHECK(mapped("1.5") == "1.5");
    // The constant %pi keeps its % — only the $ sigil is a wrapper.
    CHECK(mapped("$%PI") == "%pi");
    CHECK(mapped("$%E") == "%e");
    CHECK(mapped("$%I") == "%i");
    // Maxima's booleans are the Lisp ones.
    CHECK(mapped("T") == "true");
    CHECK(mapped("NIL") == "false");
}

TEST_CASE("exact rationals stay exact") {
    const Expr value = mapExpr("((RAT SIMP) 11 15)");
    CHECK(value.kind() == Kind::Rational);
    CHECK(value.numerator() == 11);
    CHECK(value.denominator() == 15);
    CHECK(value.str() == "11/15");
}

TEST_CASE("a large integer arrives as a number, exactly") {
    // 30!. proxima::Integer is unbounded, so this is an Integer node rather than
    // the Opaque text it used to become.
    const Expr value = mapExpr("265252859812191058636308480000000");
    CHECK(value.kind() == Kind::Integer);
    CHECK(value.str() == "265252859812191058636308480000000");
}

TEST_CASE("a rational with large parts is still a rational") {
    // Previously this degenerated into an Opaque blob as soon as either part
    // exceeded 64 bits, which made the result unusable for anything but
    // printing.
    // 31 rather than 3: 30! is divisible by 3, so that would reduce away and
    // test the wrong thing. 31 is prime and larger than 30, so it does not.
    const Expr value = mapExpr(
        "((RAT SIMP) 265252859812191058636308480000000 31)");
    CHECK(value.kind() == Kind::Rational);
    CHECK(value.numerator().toString() == "265252859812191058636308480000000");
    CHECK(value.denominator() == proxima::Integer(31));

    SUBCASE("and a reducible one is reduced") {
        const Expr reducible = mapExpr(
            "((RAT SIMP) 265252859812191058636308480000000 3)");
        CHECK(reducible.numerator().toString()
              == "88417619937397019545436160000000");
        CHECK(reducible.denominator() == proxima::Integer(1));
    }
}

TEST_CASE("operators map to typed nodes") {
    CHECK(mapExpr("((MPLUS SIMP) 1 $X)").kind() == Kind::Add);
    CHECK(mapExpr("((MTIMES SIMP) 2 $X)").kind() == Kind::Mul);
    CHECK(mapExpr("((MEXPT SIMP) $X 2)").kind() == Kind::Pow);

    CHECK(mapped("((MPLUS SIMP) 1 $X)") == "1 + x");
    CHECK(mapped("((MTIMES SIMP) 2 $X ((%SIN SIMP) $X))") == "2*x*sin(x)");
    CHECK(mapped("((MEXPT SIMP) $X 2)") == "x^2");
}

TEST_CASE("extra head flags are ignored") {
    // Heads carry simplification flags beyond SIMP; only the first element
    // names the operator.
    CHECK(mapExpr("((MEXPT SIMP RATSIMP) $X 2)").kind() == Kind::Pow);
    CHECK(mapped("((MEXPT SIMP RATSIMP) $X 2)") == "x^2");
}

TEST_CASE("relations map to typed nodes rather than applications") {
    CHECK(mapExpr("((MEQUAL SIMP) $X 1)").kind() == Kind::Relation);
    CHECK(mapExpr("((MEQUAL SIMP) $X 1)").relationOp() == proxima::RelOp::Equal);

    CHECK(mapped("((MEQUAL SIMP) $X 1)") == "x = 1");
    CHECK(mapped("((MGREATERP SIMP) $X 0)") == "x > 0");
    CHECK(mapped("((MLESSP SIMP) $X 0)") == "x < 0");
    CHECK(mapped("((MLEQP SIMP) $X 0)") == "x <= 0");
    CHECK(mapped("((MGEQP SIMP) $X 0)") == "x >= 0");
    CHECK(mapped("((MNOTEQUAL SIMP) $X 0)") == "x # 0");
}

TEST_CASE("both function-head spellings lose their sigil") {
    // % for Maxima's own operators and nouns, $ for user-defined names.
    CHECK(mapped("((%SIN SIMP) $X)") == "sin(x)");
    CHECK(mapped("(($F SIMP) $X $Y)") == "f(x, y)");
    CHECK(mapped("((%BESSEL_J SIMP) 0 $X)") == "bessel_j(0, x)");
}

TEST_CASE("unmodelled applications survive as uninterpreted functions") {
    // The escape hatch. None of these need a typed node to round-trip.
    CHECK(mapped("(($MATRIX SIMP) ((MLIST SIMP) 1 2) ((MLIST SIMP) 3 4))")
          == "matrix([1, 2], [3, 4])");
    CHECK(mapped("((MABS SIMP) $X)") == "abs(x)");
    CHECK(mapped("((%GAMMA_INCOMPLETE SIMP) ((RAT SIMP) 1 3) $X)")
          == "gamma_incomplete(1/3, x)");
}

TEST_CASE("a noun whose display name differs from its head is renamed") {
    // 'diff(f(x), x) has internal head %DERIVATIVE. Printing derivative(...)
    // would be an undefined function rather than a derivative, and dropping the
    // quote would ask Maxima to evaluate it.
    CHECK(mapped("((%DERIVATIVE SIMP) (($F SIMP) $X) $X 1)")
          == "'diff(f(x), x, 1)");
}

TEST_CASE("lists print in the only syntax Maxima has for them") {
    CHECK(mapped("((MLIST SIMP) $A $B $C)") == "[a, b, c]");
    CHECK(mapped("((MLIST SIMP))") == "[]");
    CHECK(mapped("((MLIST SIMP) ((MEQUAL SIMP) $X -1) ((MEQUAL SIMP) $X 1))")
          == "[x = -1, x = 1]");
}

TEST_CASE("a bigfloat keeps its exact value, though not its bigfloat-ness") {
    // bfloat(%pi): mantissa * 2^(exponent - bits(mantissa)). There is no
    // arbitrary-precision float to map onto, so it becomes the exact rational
    // it equals. Nothing is rounded; sending it back gives a rational.
    const Expr value = mapExpr("((BIGFLOAT SIMP 56) 56593902016227522 2)");
    CHECK(value.kind() == Kind::Opaque);
    CHECK(value.str() == "(56593902016227522/2^54)");
}

TEST_CASE("strings become Maxima source text, re-escaped") {
    CHECK(mapped(R"("a string")") == R"("a string")");
    CHECK(mapped(R"("say \"hi\"")") == R"("say \"hi\"")");
}

TEST_CASE("a malformed term is rejected rather than half-mapped") {
    CHECK_THROWS_AS(fromMaxima(parseSExpr("()")), proxima::ParseError);
    CHECK_THROWS_AS(fromMaxima(parseSExpr("((42 SIMP) 1)")), proxima::ParseError);

    SUBCASE("including a rational that is not one") {
        // These used to become Opaque text, "(1/0)" and "($X/2)", and so
        // survived as something that looked like an answer.
        CHECK_THROWS_AS(fromMaxima(parseSExpr("((RAT SIMP) 1 0)")), proxima::ParseError);
        CHECK_THROWS_AS(fromMaxima(parseSExpr("((RAT SIMP) $X 2)")), proxima::ParseError);
        CHECK_THROWS_AS(fromMaxima(parseSExpr("((RAT SIMP) 1)")), proxima::ParseError);
    }
}

TEST_CASE("every recorded Maxima reply maps to something printable") {
    // Same golden file the reader is checked against. Here the point is that
    // nothing Maxima actually emits reaches a mapping hole: every recorded form
    // produces an Expr, and every Expr produces non-empty Maxima text.
    std::ifstream golden(std::string(PROXIMA_GOLDEN_DIR) + "/internal_forms.tsv");
    REQUIRE_MESSAGE(golden.is_open(), "cannot open the golden transcript file");

    int cases = 0;
    std::string line;
    while (std::getline(golden, line)) {
        if (line.empty() || line.front() == '#') {
            continue;
        }
        std::istringstream fields(line);
        std::string expression, status, form;
        std::getline(fields, expression, '\t');
        std::getline(fields, status, '\t');
        std::getline(fields, form);
        if (status != "OK" || form.empty()) {
            continue;
        }

        CAPTURE(expression);
        CAPTURE(form);
        Expr mappedExpr;
        REQUIRE_NOTHROW(mappedExpr = fromMaxima(parseSExpr(form)));
        CHECK_FALSE(mappedExpr.str().empty());
        ++cases;
    }
    CHECK(cases >= 35);
}
