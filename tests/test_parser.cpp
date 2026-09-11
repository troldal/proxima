// The offline infix parser. No kernel, no child process — that is its reason
// for existing.

#include <doctest/doctest.h>

#include <mx/errors.hpp>
#include <mx/expr.hpp>
#include <mx/functions.hpp>
#include <mx/ops.hpp>
#include <mx/symbol.hpp>

#include <string>

using mx::Expr;
using mx::Kind;
using mx::Symbol;

TEST_CASE("atoms") {
    CHECK(Expr::parse("42") == Expr(42));
    CHECK(Expr::parse("x") == Expr::symbol("x"));
    CHECK(Expr::parse("%pi") == Expr::symbol("%pi"));
    CHECK(Expr::parse("x_1") == Expr::symbol("x_1"));
    CHECK(Expr::parse("1.5") == Expr(1.5));
    CHECK(Expr::parse("1.0e-3") == Expr(1.0e-3));
    CHECK(Expr::parse(".5") == Expr(0.5));
}

TEST_CASE("exactness is preserved") {
    // The single reason not to reach for a double-based expression library:
    // 1/3 must stay a third.
    const Expr third = Expr::parse("1/3");
    REQUIRE(third.kind() == Kind::Rational);
    CHECK(third.numerator() == 1);
    CHECK(third.denominator() == 3);

    // And an inexact literal stays inexact.
    CHECK(Expr::parse("1.0/3").kind() == Kind::Real);
}

TEST_CASE("an integer of any size is read exactly") {
    const Expr big = Expr::parse("265252859812191058636308480000000");
    CHECK(big.kind() == Kind::Integer);
    CHECK(big.str() == "265252859812191058636308480000000");

    // And it is a number, so arithmetic on it works rather than being
    // deferred to Maxima.
    CHECK((big * Expr(2)).str() == "530505719624382117272616960000000");
}

TEST_CASE("arithmetic and precedence") {
    const Symbol x("x");
    const Symbol y("y");

    CHECK(Expr::parse("x + 1") == Expr(x) + 1);
    CHECK(Expr::parse("x - 1") == Expr(x) - 1);
    CHECK(Expr::parse("2*x") == 2 * Expr(x));
    CHECK(Expr::parse("x/2") == Expr(x) / Expr(2));

    SUBCASE("multiplication binds tighter than addition") {
        CHECK(Expr::parse("1 + 2*x") == Expr(1) + 2 * Expr(x));
        CHECK(Expr::parse("(1 + 2)*x") == 3 * Expr(x));
    }
    SUBCASE("addition and multiplication are left-associative") {
        CHECK(Expr::parse("x - y - 1") == Expr(x) - Expr(y) - 1);
        CHECK(Expr::parse("8/4/2") == Expr(1));
    }
}

TEST_CASE("the two precedences that surprise people") {
    const Symbol x("x");

    SUBCASE("^ is right-associative") {
        // x^2^3 is x^8, not (x^2)^3 which would be x^6.
        CHECK(Expr::parse("x^2^3") == pow(Expr(x), pow(Expr(2), 3)));
        CHECK(Expr::parse("2^3^2") == Expr::parse("2^(3^2)"));
        CHECK_FALSE(Expr::parse("2^3^2") == Expr::parse("(2^3)^2"));
    }
    SUBCASE("unary minus binds looser than ^") {
        // -x^2 is -(x^2), not (-x)^2.
        CHECK(Expr::parse("-x^2") == -pow(Expr(x), 2));
        CHECK_FALSE(Expr::parse("-x^2") == pow(-Expr(x), 2));
        // -(3^2), not (-3)^2. Nothing folds the power, so this is the
        // negation of a power rather than the integer -9.
        CHECK(Expr::parse("-3^2") == -pow(Expr(3), 2));
    }
}

TEST_CASE("unary operators") {
    const Symbol x("x");
    CHECK(Expr::parse("-5") == Expr(-5));
    CHECK(Expr::parse("+5") == Expr(5));
    CHECK(Expr::parse("-x") == -Expr(x));
    CHECK(Expr::parse("--5") == Expr(5));
    CHECK(Expr::parse("1 - -2") == Expr(3));
}

TEST_CASE("function application") {
    const Symbol x("x");

    CHECK(Expr::parse("sin(x)") == mx::sin(Expr(x)));
    CHECK(Expr::parse("f(x, y)")
          == Expr::function("f", {Expr(x), Expr::symbol("y")}));
    CHECK(Expr::parse("f()") == Expr::function("f", {}));
    // Nested, and with an expression as an argument.
    CHECK(Expr::parse("sin(cos(x + 1))")
          == mx::sin(mx::cos(Expr(x) + 1)));
}

TEST_CASE("lists use the bracket syntax Maxima does") {
    CHECK(Expr::parse("[1, 2, 3]")
          == Expr::function("list", {Expr(1), Expr(2), Expr(3)}));
    CHECK(Expr::parse("[]") == Expr::function("list", {}));
    // And print back the same way.
    CHECK(Expr::parse("[1, 2]").str() == "[1, 2]");
}

TEST_CASE("relations") {
    const Symbol x("x");

    CHECK(Expr::parse("x = 1") == eq(Expr(x), Expr(1)));
    CHECK(Expr::parse("x # 1") == ne(Expr(x), Expr(1)));
    CHECK(Expr::parse("x < 1") == lt(Expr(x), Expr(1)));
    CHECK(Expr::parse("x <= 1") == le(Expr(x), Expr(1)));
    CHECK(Expr::parse("x > 1") == gt(Expr(x), Expr(1)));
    CHECK(Expr::parse("x >= 1") == ge(Expr(x), Expr(1)));

    SUBCASE("and bind looser than arithmetic") {
        CHECK(Expr::parse("x + 1 = 2*x") == eq(Expr(x) + 1, 2 * Expr(x)));
    }
}

TEST_CASE("postfix factorial") {
    CHECK(Expr::parse("5!") == Expr::function("factorial", {Expr(5)}));
    // Binds tighter than multiplication.
    CHECK(Expr::parse("2*3!")
          == 2 * Expr::function("factorial", {Expr(3)}));
}

TEST_CASE("strings become source text, as they do from Maxima") {
    const Expr text = Expr::parse(R"("a string")");
    CHECK(text.kind() == Kind::Opaque);
    CHECK(text.str() == R"("a string")");
    CHECK(Expr::parse(R"("say \"hi\"")").str() == R"("say \"hi\"")");
}

TEST_CASE("whitespace is irrelevant") {
    CHECK(Expr::parse("  x   +\t1\n") == Expr::parse("x+1"));
}

TEST_CASE("malformed input is rejected, with the offset") {
    CHECK_THROWS_AS(Expr::parse(""), mx::ParseError);
    CHECK_THROWS_AS(Expr::parse("x +"), mx::ParseError);
    CHECK_THROWS_AS(Expr::parse("(x"), mx::ParseError);
    CHECK_THROWS_AS(Expr::parse("x)"), mx::ParseError);
    CHECK_THROWS_AS(Expr::parse("f(x"), mx::ParseError);
    CHECK_THROWS_AS(Expr::parse("[1, 2"), mx::ParseError);
    CHECK_THROWS_AS(Expr::parse(R"("unterminated)"), mx::ParseError);
    CHECK_THROWS_AS(Expr::parse("x @ y"), mx::ParseError);
    // Two expressions where one was promised.
    CHECK_THROWS_AS(Expr::parse("1 2"), mx::ParseError);

    SUBCASE("and the message says where") {
        try {
            Expr::parse("x + @");
            FAIL("expected a ParseError");
        } catch (const mx::ParseError &e) {
            CHECK(std::string(e.what()).find("offset 4") != std::string::npos);
        }
    }
}

TEST_CASE("statements are not expressions, and are refused") {
    // Assignment, definition and quoting are Maxima *programs*. Refusing them
    // here is the boundary that keeps this a parser for expressions; mx::parse
    // hands such things to Maxima itself.
    CHECK_THROWS_AS(Expr::parse("a: 7"), mx::ParseError);
    CHECK_THROWS_AS(Expr::parse("f(x) := x^2"), mx::ParseError);
    CHECK_THROWS_AS(Expr::parse("'diff(f(x), x)"), mx::ParseError);
}

TEST_CASE("what is parsed prints back to the same thing") {
    // Not textual equality — canonical order and spacing differ — but parsing
    // the printed form must give the same expression.
    for (const char *source : {
             "x + 1",
             "2*x*sin(x)",
             "x^2 - 3*x + 2",
             "1/3 + 2/5",
             "(x + 1)^2",
             "x^2^3",
             "-x^2",
             "f(x, y, 1/2)",
             "[1, x, sin(x)]",
             "x = 1",
             "x + 1 >= 2*y",
         }) {
        const std::string text = source;
        CAPTURE(text);
        const Expr once = Expr::parse(source);
        CHECK(Expr::parse(once.str()) == once);
    }
}

TEST_SUITE("maxima") {

TEST_CASE("the offline parser agrees with Maxima's own") {
    // The subset it covers must *mean* the same thing to both. Where they
    // disagree, the offline one is wrong by definition — Maxima's parser is the
    // specification for the syntax it accepts.
    //
    // Meaning, not structure. mx::parse evaluates as it parses — it answers
    // x^2^3 with x^8 and 5! with 120 — whereas Expr::parse only builds and
    // normalises. Comparing the two directly would be comparing a parse against
    // an evaluation. So both sides are put through Maxima: its reading of the
    // original text, against its reading of what the offline parser printed.
    for (const char *source : {
             "x + 1",
             "2*x*sin(x)",
             "x^2 - 3*x + 2",
             "1/3 + 2/5",
             "(x + 1)^2",
             "x^2^3",
             "-x^2",
             "-3^2",
             "8/4/2",
             "x - y - 1",
             "1/2*x",
             "sin(cos(x))",
             "f(x, y)",
             "[1, 2, 3]",
             "x = 1",
             "x >= 2*y",
             "5!",
             "1.5*x",
         }) {
        const std::string text = source;
        CAPTURE(text);
        const auto viaMaxima = mx::parse(source);
        REQUIRE(viaMaxima.has_value());

        const Expr offline = Expr::parse(source);
        const auto offlineViaMaxima = mx::parse(offline.str());
        REQUIRE(offlineViaMaxima.has_value());

        INFO("offline: ", offline.str(), "   maxima: ", viaMaxima->str());
        CHECK(*offlineViaMaxima == *viaMaxima);
    }
}

TEST_CASE("an offline-parsed expression is usable without a kernel first") {
    // The point of having it: building expressions from text needs no Maxima
    // running, even though evaluating them does.
    const Symbol x("x");
    const Expr f = Expr::parse("x^2*sin(x)");

    const auto integral = mx::integrate(f, x);
    REQUIRE(integral.has_value());
    CHECK(mx::simplify(mx::diff(*integral, x)) == f);
}

} // TEST_SUITE("maxima")
