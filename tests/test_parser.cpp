// The offline infix parser. No kernel, no child process — that is its reason
// for existing.

#include <doctest/doctest.h>

#include <proxima/errors.hpp>
#include <proxima/expr.hpp>
#include <proxima/functions.hpp>
#include <proxima/ops.hpp>
#include <proxima/symbol.hpp>

#include <cstddef>
#include <string>
#include <vector>

using proxima::Expr;
using proxima::Kind;
using proxima::Symbol;

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

TEST_CASE("a long sum or product is built once, not once per operator") {
    // Each operator used to re-normalise everything to its left, so parsing
    // took time quadratic in the number of terms: nine seconds for 4000. A
    // regression shows up here as a slow test rather than a failing one.
    const std::size_t n = 20000;
    std::vector<Expr> terms;
    std::vector<Expr> negated;
    std::string sum;
    std::string alternating;
    std::string product;
    for (std::size_t i = 0; i < n; ++i) {
        const Expr term = Expr::symbol("x" + std::to_string(i));
        terms.push_back(term);
        negated.push_back(i % 2 == 0 ? term : -term);
        sum += (i ? " + x" : "x") + std::to_string(i);
        alternating += (i == 0 ? "x" : i % 2 == 0 ? " + x" : " - x") + std::to_string(i);
        product += (i ? " * x" : "x") + std::to_string(i);
    }
    CHECK(Expr::parse(sum) == Expr::add(terms));
    CHECK(Expr::parse(alternating) == Expr::add(negated));
    CHECK(Expr::parse(product) == Expr::mul(terms));
}

TEST_CASE("runs of operators mean what chained operators mean") {
    const Symbol a("a");
    const Symbol b("b");
    const Symbol c("c");
    const Symbol d("d");
    CHECK(Expr::parse("a - b + c - d") == Expr(a) - b + c - d);
    CHECK(Expr::parse("a / b * c / d") == Expr(a) / b * c / d);
    CHECK(Expr::parse("a*b + c/d - a^2*b") == Expr(a) * b + Expr(c) / d - pow(Expr(a), 2) * b);
    CHECK(Expr::parse("-a - b") == -Expr(a) - b);
    CHECK(Expr::parse("2/3*a/4") == Expr::rational(1, 6) * a);
    CHECK(Expr::parse("a/0.5/2") == Expr(a) / Expr(0.5) / Expr(2));
    CHECK(Expr::parse("1/0 + a") == Expr(a) + Expr(1) / Expr(0));
    CHECK(Expr::parse("a + b < c * d - 1") == lt(Expr(a) + b, Expr(c) * d - 1));
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

TEST_CASE("** is ^, as Maxima reads it") {
    CHECK(Expr::parse("x**2") == Expr::parse("x^2"));
    // The same operator in every respect: right-associative, binding tighter
    // than unary minus.
    CHECK(Expr::parse("2**3**2") == Expr::parse("2^(3^2)"));
    CHECK(Expr::parse("-x**2") == Expr::parse("-(x^2)"));
    CHECK(Expr::parse("x**2*y") == Expr::parse("x^2*y"));
    // Only when the stars are adjacent, as in Maxima's own lexer.
    CHECK_THROWS_AS(static_cast<void>(Expr::parse("x * * 2")), proxima::ParseError);
}

TEST_CASE("a number beyond a double's range is refused, and says so") {
    // It used to be reported as a malformed number, which it is not.
    for (const char *source : {"1e400", "-1e400", "2*1e999"}) {
        CAPTURE(source);
        CHECK_THROWS_WITH_AS(static_cast<void>(Expr::parse(source)),
                             doctest::Contains("out of the range of a double"),
                             proxima::ParseError);
    }
    CHECK(Expr::parse("1e300") == Expr(1e300));
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

    CHECK(Expr::parse("sin(x)") == proxima::sin(Expr(x)));
    CHECK(Expr::parse("f(x, y)")
          == Expr::function("f", {Expr(x), Expr::symbol("y")}));
    CHECK(Expr::parse("f()") == Expr::function("f", {}));
    // Nested, and with an expression as an argument.
    CHECK(Expr::parse("sin(cos(x + 1))")
          == proxima::sin(proxima::cos(Expr(x) + 1)));
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

    SUBCASE("but do not chain") {
        // Maxima's parser: "Found LOGICAL expression where ALGEBRAIC expression
        // expected". Accepting these as (a < b) < c gave text a meaning Maxima
        // never gives it.
        for (const char *source :
             {"a < b < c", "a = b = c", "a # b # c", "a < b = c", "a = b + 1 < c"}) {
            CAPTURE(std::string(source));
            CHECK_THROWS_AS(Expr::parse(source), proxima::ParseError);
        }
        try {
            Expr::parse("a = b = c");
            FAIL("expected a ParseError");
        } catch (const proxima::ParseError &e) {
            // Where Maxima puts its caret: the second relation.
            CHECK(std::string(e.what()).find("offset 6") != std::string::npos);
        }
    }

    SUBCASE("unless parenthesised, which Maxima accepts too") {
        const Symbol a("a");
        const Symbol b("b");
        const Symbol c("c");
        CHECK(Expr::parse("(a < b) < c") == lt(lt(Expr(a), Expr(b)), Expr(c)));
        CHECK(Expr::parse("a = (b = c)") == eq(Expr(a), eq(Expr(b), Expr(c))));
        CHECK(Expr::parse("f(a < b)")
              == Expr::function("f", {lt(Expr(a), Expr(b))}));
        CHECK(Expr::parse("[a = b, b < c]")
              == Expr::function("list", {eq(Expr(a), Expr(b)), lt(Expr(b), Expr(c))}));

        // And print with the parentheses kept, or the printed text would be
        // exactly what is refused above.
        for (const char *source : {"(a < b) < c", "a = (b = c)", "(a = b) # (b = c)"}) {
            CAPTURE(std::string(source));
            const Expr once = Expr::parse(source);
            CAPTURE(once.str());
            CHECK(Expr::parse(once.str()) == once);
        }
    }
}

TEST_CASE("postfix factorial") {
    CHECK(Expr::parse("5!") == Expr::function("factorial", {Expr(5)}));
    // Binds tighter than multiplication.
    CHECK(Expr::parse("2*3!")
          == 2 * Expr::function("factorial", {Expr(3)}));
}

TEST_CASE("!! is the double factorial, not a factorial taken twice") {
    // Each expectation is what Maxima's own reader produces for the same text.
    const Expr x = Expr::symbol("x");
    const auto fact = [](Expr e) { return Expr::function("factorial", {std::move(e)}); };
    const auto dfact
        = [](Expr e) { return Expr::function("double_factorial", {std::move(e)}); };

    CHECK(Expr::parse("x!!") == dfact(x));
    CHECK(Expr::parse("2*x!!") == 2 * dfact(x));

    SUBCASE("read greedily, so a space is what separates two factorials") {
        CHECK(Expr::parse("x!!!") == fact(dfact(x)));
        CHECK(Expr::parse("x! !") == fact(fact(x)));
        CHECK(Expr::parse("(x!)!") == fact(fact(x)));
        CHECK(Expr::parse("(x!)!!") == dfact(fact(x)));
    }
    SUBCASE("and binding as tightly as !") {
        CHECK(Expr::parse("x^2!!") == pow(x, dfact(Expr(2))));
    }
    SUBCASE("printed as the function name Maxima reads back") {
        CHECK(Expr::parse("x!!").str() == "double_factorial(x)");
    }
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
    CHECK_THROWS_AS(Expr::parse(""), proxima::ParseError);
    CHECK_THROWS_AS(Expr::parse("x +"), proxima::ParseError);
    CHECK_THROWS_AS(Expr::parse("(x"), proxima::ParseError);
    CHECK_THROWS_AS(Expr::parse("x)"), proxima::ParseError);
    CHECK_THROWS_AS(Expr::parse("f(x"), proxima::ParseError);
    CHECK_THROWS_AS(Expr::parse("[1, 2"), proxima::ParseError);
    CHECK_THROWS_AS(Expr::parse(R"("unterminated)"), proxima::ParseError);
    CHECK_THROWS_AS(Expr::parse("x @ y"), proxima::ParseError);
    // Two expressions where one was promised.
    CHECK_THROWS_AS(Expr::parse("1 2"), proxima::ParseError);

    SUBCASE("and the message says where") {
        try {
            Expr::parse("x + @");
            FAIL("expected a ParseError");
        } catch (const proxima::ParseError &e) {
            CHECK(std::string(e.what()).find("offset 4") != std::string::npos);
        }
    }
}

TEST_CASE("text nested too deep is refused, not a stack overflow") {
    // Found by fuzzing: 200,000 opening parentheses, unary minuses or a chain
    // of powers each recursed once per level and crashed the process.
    const std::size_t levels = 200'000;
    const std::string parens = std::string(levels, '(') + "x" + std::string(levels, ')');
    const std::string minuses = std::string(levels, '-') + "x";
    std::string powers = "x";
    for (std::size_t i = 0; i < levels; ++i) {
        powers += "^x";
    }

    for (const std::string *source :
         std::initializer_list<const std::string *>{&parens, &minuses, &powers}) {
        CHECK_THROWS_WITH_AS(static_cast<void>(Expr::parse(*source)),
                             doctest::Contains("nested too deep"), proxima::ParseError);
    }

    SUBCASE("while ordinary nesting, and a long flat sum, still parse") {
        // In every build: a Debug build on Windows spends several times the
        // stack per level that a Release build does, and has a 1 MB stack. A
        // fixed limit of 1000 levels overflowed there before it was reached.
        const std::size_t modest = 100;
        CHECK_NOTHROW(static_cast<void>(
            Expr::parse(std::string(modest, '(') + "x" + std::string(modest, ')'))));
        CHECK_NOTHROW(static_cast<void>(Expr::parse(std::string(modest, '-') + "x")));
        // Sums loop rather than recurse, so length is not depth. Longer than
        // the limit, but not much: each + rebuilds the sum so far.
        std::string sum = "x";
        for (int i = 0; i < 2'000; ++i) {
            sum += "+x";
        }
        CHECK_NOTHROW(static_cast<void>(Expr::parse(sum)));
    }
}

TEST_CASE("statements are not expressions, and are refused") {
    // Assignment, definition and quoting are Maxima *programs*. Refusing them
    // here is the boundary that keeps this a parser for expressions; proxima::parse
    // hands such things to Maxima itself.
    CHECK_THROWS_AS(Expr::parse("a: 7"), proxima::ParseError);
    CHECK_THROWS_AS(Expr::parse("f(x) := x^2"), proxima::ParseError);
    CHECK_THROWS_AS(Expr::parse("'diff(f(x), x)"), proxima::ParseError);
}

TEST_CASE("every Maxima operator outside the subset is refused") {
    // The subset is arithmetic, comparisons, application, lists and strings,
    // with !, !! and ** read as Maxima reads them. Each of these is Maxima
    // syntax it does not take — proxima::parse hands such text to Maxima — and each
    // must be refused, not read as something else.
    for (const char *source : {
             "a: 7", "a :: 7", "f(x) := x^2", "f(x) ::= x",  // assignment, definitions
             "'x", "''x",                                    // quoting
             "a . b", "a ^^ 2",                              // non-commutative product, power
             "a[1]",                                         // subscripts
             "a and b", "a or b", "not a",                   // logic
             "if a then b else c", "for i thru 3 do x",      // control flow
             "x;", "x$",                                     // statement terminators
             "?print(x)",                                    // a Lisp escape
             "a ~ b", "a -> b", "a | b",                     // operators Maxima does not have
         }) {
        CAPTURE(std::string(source));
        CHECK_THROWS_AS(static_cast<void>(Expr::parse(source)), proxima::ParseError);
    }
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
             "x!!",
             "x!!!",
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
    // Meaning, not structure. proxima::parse evaluates as it parses — it answers
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
             "x**2",
             "8/4/2",
             "x - y - 1",
             "1/2*x",
             "sin(cos(x))",
             "f(x, y)",
             "[1, 2, 3]",
             "x = 1",
             "x >= 2*y",
             "5!",
             "5!!",
             "x!!",
             "x!!!",
             "x! !",
             "2*x!!",
             "1.5*x",
         }) {
        const std::string text = source;
        CAPTURE(text);
        const auto via_maxima = proxima::parse(source);
        REQUIRE(via_maxima.has_value());

        const Expr offline = Expr::parse(source);
        const auto offline_via_maxima = proxima::parse(offline.str());
        REQUIRE(offline_via_maxima.has_value());

        INFO("offline: ", offline.str(), "   maxima: ", via_maxima->str());
        CHECK(*offline_via_maxima == *via_maxima);
    }
}

TEST_CASE("an offline-parsed expression is usable without a kernel first") {
    // The point of having it: building expressions from text needs no Maxima
    // running, even though evaluating them does.
    const Symbol x("x");
    const Expr f = Expr::parse("x^2*sin(x)");

    const auto integral = proxima::integrate(f, x);
    REQUIRE(integral.has_value());
    CHECK(proxima::simplify(proxima::diff(*integral, x)) == f);
}

} // TEST_SUITE("maxima")
