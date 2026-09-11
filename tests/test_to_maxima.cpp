// Expr -> Maxima internal form: the outbound half of the translation layer.
//
// The pure tests pin the encoding. The Maxima-backed ones are the point of
// the whole exercise: an expression goes out as structure and comes back as
// structure, so the things that used to stall the kernel — a `$` in an
// Opaque, a space in a symbol name — are now either answered or refused with
// a message, in milliseconds.

#include <doctest/doctest.h>

#include "kernel/session.hpp"
#include "wire/from_maxima.hpp"
#include "wire/sexpr.hpp"
#include "wire/to_maxima.hpp"

#include <mx/context.hpp>
#include <mx/errors.hpp>
#include <mx/expr.hpp>
#include <mx/functions.hpp>
#include <mx/kernel.hpp>
#include <mx/ops.hpp>
#include <mx/symbol.hpp>

#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

using mx::Expr;
using mx::Kind;
using mx::Symbol;
using mx::detail::encodeMaximaName;
using mx::detail::fromMaxima;
using mx::detail::parseSExpr;
using mx::detail::Payload;
using mx::detail::stringLiteral;
using mx::detail::toMaxima;

TEST_CASE("symbol names are encoded as Maxima stores them") {
    // The exact inverse of decodeMaximaName, which test_from_maxima covers.
    CHECK(encodeMaximaName("x") == "$X");
    CHECK(encodeMaximaName("x_1") == "$X_1");
    CHECK(encodeMaximaName("%pi") == "$%PI");
    CHECK(encodeMaximaName("alpha") == "$ALPHA");

    SUBCASE("a name the Lisp reader would alter is bar-quoted") {
        // Upper case in the user's name is lower case in Maxima's, and the
        // reader upcases anything unquoted.
        CHECK(encodeMaximaName("X") == "|$x|");
        CHECK(encodeMaximaName("xY") == "|$xY|");
        // Spaces and punctuation are syntax to the reader; quoted, they are
        // just characters, which is what makes such a symbol safe to send.
        CHECK(encodeMaximaName("x y") == "|$X Y|");
        CHECK(encodeMaximaName("a(b") == "|$A(B|");
        // An empty name is the bare sigil, which is a valid symbol and
        // decodes back to the empty name.
        CHECK(encodeMaximaName("") == "$");
    }
    SUBCASE("the quoting characters themselves are escaped") {
        CHECK(encodeMaximaName("a|b") == "|$A\\|B|");
        CHECK(encodeMaximaName("a\\b") == "|$A\\\\B|");
    }
}

TEST_CASE("string literals escape exactly what both readers need") {
    CHECK(stringLiteral("plain") == "\"plain\"");
    CHECK(stringLiteral("say \"hi\"") == "\"say \\\"hi\\\"\"");
    CHECK(stringLiteral("back\\slash") == "\"back\\\\slash\"");
    CHECK(stringLiteral("") == "\"\"");
}

TEST_CASE("atoms") {
    CHECK(toMaxima(Expr(42)) == "42");
    CHECK(toMaxima(Expr(-7)) == "-7");
    CHECK(toMaxima(Expr(mx::Integer("265252859812191058636308480000000")))
          == "265252859812191058636308480000000");
    CHECK(toMaxima(Expr::symbol("x")) == "$X");
    CHECK(toMaxima(Expr::symbol("true")) == "T");
    CHECK(toMaxima(Expr::symbol("false")) == "NIL");

    SUBCASE("a rational is a quotient, for Maxima to canonicalise itself") {
        CHECK(toMaxima(Expr::rational(11, 15)) == "((MQUOTIENT) 11 15)");
        CHECK(toMaxima(Expr::rational(-1, 3)) == "((MQUOTIENT) -1 3)");
    }
    SUBCASE("a real always reads as a double") {
        CHECK(toMaxima(Expr(1.5)) == "1.5d0");
        CHECK(toMaxima(Expr(2.0)) == "2.0d0");
        CHECK(toMaxima(Expr(1e300)) == "1d+300");
        CHECK(toMaxima(Expr(-0.0)) == "-0.0d0");
    }
    SUBCASE("infinities have Maxima's names, and NaN has none") {
        CHECK(toMaxima(Expr(std::numeric_limits<double>::infinity())) == "$INF");
        CHECK(toMaxima(Expr(-std::numeric_limits<double>::infinity())) == "$MINF");
        CHECK_THROWS_AS(toMaxima(Expr(std::nan(""))), mx::Error);
    }
}

TEST_CASE("operators are emitted without simplification flags") {
    // (MPLUS), not (MPLUS SIMP): Maxima simplifies what it is handed rather
    // than being told it already has been.
    const Symbol x("x");
    CHECK(toMaxima(x + 1) == "((MPLUS) 1 $X)");
    CHECK(toMaxima(2 * x) == "((MTIMES) 2 $X)");
    CHECK(toMaxima(pow(x, 2)) == "((MEXPT) $X 2)");
    CHECK(toMaxima(x / 3) == "((MTIMES) ((MQUOTIENT) 1 3) $X)");
}

TEST_CASE("function heads take the sigil Maxima's own parser would give them") {
    const Symbol x("x");
    CHECK(toMaxima(mx::sin(x)) == "(($SIN) $X)");
    CHECK(toMaxima(Expr::function("f", {x, Expr(1)})) == "(($F) $X 1)");
    CHECK(toMaxima(Expr::function("myFunc", {x})) == "((|$myFunc|) $X)");

    SUBCASE("except the heads Maxima spells differently") {
        CHECK(toMaxima(Expr::function("list", {Expr(1), Expr(2)}))
              == "((MLIST) 1 2)");
        CHECK(toMaxima(mx::abs(x)) == "((MABS) $X)");
        CHECK(toMaxima(Expr::function("factorial", {Expr(5)}))
              == "((MFACTORIAL) 5)");
        CHECK(toMaxima(Expr::function("'diff", {Expr::function("f", {x}), x, Expr(1)}))
              == "((%DERIVATIVE) (($F) $X) $X 1)");
    }
    SUBCASE("a quoted head is a noun") {
        CHECK(toMaxima(Expr::function("'integrate", {x, x}))
              == "((%INTEGRATE) $X $X)");
    }
}

TEST_CASE("relations") {
    const Symbol x("x");
    CHECK(toMaxima(eq(x, 1)) == "((MEQUAL) $X 1)");
    CHECK(toMaxima(ne(x, 0)) == "((MNOTEQUAL) $X 0)");
    CHECK(toMaxima(lt(x, 0)) == "((MLESSP) $X 0)");
    CHECK(toMaxima(le(x, 0)) == "((MLEQP) $X 0)");
    CHECK(toMaxima(gt(x, 0)) == "((MGREATERP) $X 0)");
    CHECK(toMaxima(ge(x, 0)) == "((MGEQP) $X 0)");
}

TEST_CASE("opaque text is parsed by Maxima, inside the error trap") {
    // The escape hatch keeps working, but the text now arrives as a string
    // for Maxima to read — so a malformed one is a failure, not a stall.
    CHECK(toMaxima(Expr::opaque("x$ 0")) == "(($EVAL_STRING) \"x$ 0\")");
    CHECK(toMaxima(Expr::opaque("say \"hi\""))
          == "(($EVAL_STRING) \"say \\\"hi\\\"\")");

    SUBCASE("except that a string is simply a string") {
        // fromMaxima wraps Maxima strings as quoted Opaque text; sending them
        // back as Lisp strings keeps a string a string rather than something
        // to be parsed.
        CHECK(toMaxima(Expr::opaque("\"hello\"")) == "\"hello\"");
        CHECK(toMaxima(Expr::opaque("\"a \\\"b\\\" c\"")) == "\"a \\\"b\\\" c\"");
        // Two literals side by side are not one literal.
        CHECK(toMaxima(Expr::opaque("\"a\" \"b\""))
              == "(($EVAL_STRING) \"\\\"a\\\" \\\"b\\\"\")");
    }
}

TEST_CASE("a payload is a call on one string literal, whatever it is given") {
    // This is the type-level guarantee the protocol rests on.
    CHECK(Payload::form("((MPLUS) 1 $X)").str() == "cppread(\"((MPLUS) 1 $X)\")");
    CHECK(Payload::text("1+1").str() == "eval_string(\"1+1\")");
    // The characters that used to break the wrapper are now just characters.
    CHECK(Payload::text("1$ 2").str() == "eval_string(\"1$ 2\")");
    CHECK(Payload::text("x); quit(").str() == "eval_string(\"x); quit(\")");
    CHECK(Payload::form("(\"q\")").str() == "cppread(\"(\\\"q\\\")\")");
}

TEST_CASE("everything Maxima has ever sent can be sent back as a form") {
    // Every golden reply maps to an Expr (test_from_maxima), and every such
    // Expr must render to a form the s-expression reader accepts. A structural
    // check with no kernel: it cannot prove Maxima will like the form, but it
    // catches an encoder that emits something unreadable.
    std::ifstream golden(std::string(MX_GOLDEN_DIR) + "/internal_forms.tsv");
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
        const Expr mapped = fromMaxima(parseSExpr(form));
        std::string outbound;
        REQUIRE_NOTHROW(outbound = toMaxima(mapped));
        CAPTURE(outbound);
        CHECK_NOTHROW(parseSExpr(outbound));
        ++cases;
    }
    CHECK(cases >= 35);
}

TEST_SUITE("maxima") {

/// Sends `expr` as a form and maps the reply back.
Expr roundTrip(mx::Kernel &kernel, const Expr &expr) {
    const mx::Reply reply = kernel.evalPure(expr);
    REQUIRE_MESSAGE(reply.ok, reply.reason);
    return fromMaxima(parseSExpr(reply.value));
}

TEST_CASE("an expression survives the trip out and back unchanged") {
    // Structure out, structure back. Each of these is in a form Maxima's
    // simplifier leaves alone, so equality is exact rather than modulo
    // simplification.
    mx::Kernel kernel;
    const Symbol x("x");
    const Symbol y("y");

    const Expr corpus[] = {
        Expr(42),
        Expr(mx::Integer("265252859812191058636308480000000")),
        Expr::rational(11, 15),
        Expr(2.5),
        Expr(x),
        x + 1,
        x - y,
        2 * x,
        x / 3,
        pow(x, 2),
        pow(x, mx::Expr::rational(1, 2)),
        -x,
        mx::sin(x),
        mx::abs(x),
        Expr::function("f", {x, y}),
        Expr::function("list", {Expr(1), x, mx::sin(y)}),
        Expr::function("'diff", {Expr::function("f", {x}), x, Expr(1)}),
        eq(x, 1),
        gt(x, 0),
        mx::pi(),
        Expr::symbol("true"),
        Expr::symbol("false"),
        Expr::opaque("\"a string\""),
        // The names that were impossible to send as text.
        Expr(Symbol("X")),
        Expr(Symbol("xY")),
        Expr(Symbol("x y")),
        Expr::function("a(b", {Expr(1)}),
    };
    for (const Expr &expr : corpus) {
        CAPTURE(expr.str());
        CAPTURE(toMaxima(expr));
        CHECK(roundTrip(kernel, expr) == expr);
    }
}

TEST_CASE("Maxima simplifies what it is handed") {
    // The forms go out unflagged, and Maxima does its own arithmetic.
    mx::Kernel kernel;
    const Symbol x("x");
    CHECK(roundTrip(kernel, Expr::function("factorial", {Expr(5)})) == Expr(120));
    CHECK(roundTrip(kernel, mx::sin(Expr(0))) == Expr(0));
    CHECK(roundTrip(kernel, Expr::rational(4, 6)) == Expr::rational(2, 3));
    CHECK(mx::diff(pow(x, 3), x, 1, kernel) == 3 * pow(x, 2));
}

TEST_CASE("a symbol with a space in its name is an ordinary unknown") {
    // Before: `diff(x y^2, x y)` — a Maxima syntax error in the reader, no
    // frame, a two-minute stall. Now the name travels bar-quoted.
    mx::Kernel kernel;
    const Symbol odd("x y");
    CHECK(mx::diff(pow(odd, 2), odd, 1, kernel) == 2 * odd);
}

TEST_CASE("text Maxima cannot read is a failure, not a stall") {
    // The read error happens inside the trap now, so it costs a round trip
    // rather than Config::timeout plus a restart. The bound below is generous
    // beyond any real round trip and far below the two-minute default that a
    // regression would hit.
    mx::Kernel kernel;
    const auto within = [](auto &&call) {
        const auto start = std::chrono::steady_clock::now();
        call();
        return std::chrono::steady_clock::now() - start < std::chrono::seconds(10);
    };

    SUBCASE("through the raw text entry point") {
        mx::Reply reply;
        CHECK(within([&] { reply = kernel.eval("(1"); }));
        CHECK_FALSE(reply.ok);
        CHECK_FALSE(reply.reason.empty());
    }
    SUBCASE("through a statement terminator that used to cut the wrapper") {
        mx::Reply reply;
        CHECK(within([&] { reply = kernel.eval("1$ 2"); }));
        // Contained either way: parsed up to the terminator, or refused.
        CHECK(within([&] { reply = kernel.evalPure("2+2"); }));
        CHECK(reply.ok);
        CHECK(reply.value == "4");
    }
    SUBCASE("through an Opaque node reaching a typed operation") {
        const Symbol x("x");
        CHECK(within([&] {
            CHECK_THROWS_AS(mx::diff(Expr::opaque("(1"), x, 1, kernel),
                            mx::MaximaError);
        }));
        // And the session is intact afterwards.
        CHECK(mx::diff(pow(x, 2), x, 1, kernel) == 2 * x);
    }
}

TEST_CASE("assumptions travel as forms too") {
    // Context sends the user's predicate as structure, so the guarantee
    // covers the one path that changes Maxima's state on the user's behalf.
    mx::Kernel kernel;
    const Symbol odd("n m");
    mx::Context scope(kernel);
    CHECK_NOTHROW(scope.assume(gt(odd, 0)));
    const auto result = mx::integrate(pow(Expr(Symbol("x")), odd), Symbol("x"), kernel);
    REQUIRE(result.has_value());
    CHECK(mx::contains(*result, odd));
}

} // TEST_SUITE("maxima")
