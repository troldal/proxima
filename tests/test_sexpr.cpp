// S-expression reader tests. Entirely from string literals and the recorded
// golden file — no Maxima, no child process.

#include <doctest/doctest.h>

#include "wire/sexpr.hpp"

#include <mx/errors.hpp>

#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

using mx::detail::parseSExpr;
using mx::detail::SExpr;

TEST_CASE("atoms") {
    SUBCASE("integers") {
        const SExpr value = parseSExpr("42");
        REQUIRE(value.isInteger());
        CHECK(value.digits() == "42");
        CHECK(value.asInt64() == 42);
    }
    SUBCASE("negative integers") {
        CHECK(parseSExpr("-7").asInt64() == -7);
    }
    SUBCASE("symbols") {
        CHECK(parseSExpr("$X").isSymbol("$X"));
        CHECK(parseSExpr("MPLUS").isSymbol("MPLUS"));
        CHECK(parseSExpr("%SIN").isSymbol("%SIN"));
        // Maxima renders true and false as the Lisp booleans, not $TRUE/$FALSE.
        CHECK(parseSExpr("T").isSymbol("T"));
        CHECK(parseSExpr("NIL").isSymbol("NIL"));
    }
    SUBCASE("floats") {
        REQUIRE(parseSExpr("1.5").isReal());
        CHECK(parseSExpr("1.5").realValue() == doctest::Approx(1.5));
        CHECK(parseSExpr("1.0e-10").realValue() == doctest::Approx(1.0e-10));
        CHECK(parseSExpr("-0.25").realValue() == doctest::Approx(-0.25));
    }
    SUBCASE("strings") {
        REQUIRE(parseSExpr(R"("a string")").isString());
        CHECK(parseSExpr(R"("a string")").stringValue() == "a string");
    }
}

TEST_CASE("a float is never mistaken for an integer, or the reverse") {
    // The distinction matters downstream: an exact 2 and an inexact 2.0 are
    // different values to a CAS.
    CHECK(parseSExpr("2").isInteger());
    CHECK(parseSExpr("2.0").isReal());
    CHECK(parseSExpr("2.").isReal());
    CHECK(parseSExpr("2e3").isReal());
}

TEST_CASE("Lisp exponent markers other than e are accepted") {
    // Maxima sets *read-default-float-format* to double-float so `e` is what
    // arrives, but d/s/f/l are legal Common Lisp and would otherwise fail to
    // parse in a thoroughly mystifying way.
    CHECK(parseSExpr("1.5d0").realValue() == doctest::Approx(1.5));
    CHECK(parseSExpr("1.5s0").realValue() == doctest::Approx(1.5));
    CHECK(parseSExpr("2.0d3").realValue() == doctest::Approx(2000.0));
}

TEST_CASE("bignums are kept losslessly rather than truncated") {
    // 30! — Maxima produces these in ordinary use, well past int64. Storing the
    // digits leaves the numeric-representation decision to the layer that has
    // to make it (PLAN.md step 8) instead of silently destroying the value.
    const SExpr value = parseSExpr("265252859812191058636308480000000");
    REQUIRE(value.isInteger());
    CHECK(value.digits() == "265252859812191058636308480000000");
    CHECK_FALSE(value.asInt64().has_value());
}

TEST_CASE("int64 boundaries are handled exactly") {
    CHECK(parseSExpr("9223372036854775807").asInt64()
          == std::numeric_limits<std::int64_t>::max());
    CHECK(parseSExpr("-9223372036854775808").asInt64()
          == std::numeric_limits<std::int64_t>::min());
    // One past the top must report "does not fit", not wrap.
    CHECK_FALSE(parseSExpr("9223372036854775808").asInt64().has_value());
}

TEST_CASE("lists") {
    SUBCASE("empty") {
        const SExpr value = parseSExpr("()");
        REQUIRE(value.isList());
        CHECK(value.empty());
    }
    SUBCASE("flat") {
        const SExpr value = parseSExpr("(1 2 3)");
        REQUIRE(value.size() == 3);
        CHECK(value.at(0).asInt64() == 1);
        CHECK(value.at(2).asInt64() == 3);
    }
    SUBCASE("nested, as Maxima actually emits") {
        const SExpr value = parseSExpr("((MPLUS SIMP) 1 $X)");
        REQUIRE(value.size() == 3);

        // The head is itself a list: operator first, then flags. Flags beyond
        // SIMP do occur, so it cannot be treated as a pair.
        const SExpr &head = value.at(0);
        REQUIRE(head.isList());
        CHECK(head.at(0).isSymbol("MPLUS"));
        CHECK(head.at(1).isSymbol("SIMP"));

        CHECK(value.at(1).asInt64() == 1);
        CHECK(value.at(2).isSymbol("$X"));
    }
    SUBCASE("extra head flags") {
        const SExpr value = parseSExpr("((MEXPT SIMP RATSIMP) $X 2)");
        CHECK(value.at(0).size() == 3);
    }
}

TEST_CASE("whitespace and newlines between tokens are irrelevant") {
    const SExpr spaced = parseSExpr("(  (MPLUS\n SIMP)\t1   $X )");
    CHECK(spaced == parseSExpr("((MPLUS SIMP) 1 $X)"));
}

TEST_CASE("string escapes are undone") {
    CHECK(parseSExpr(R"("say \"hi\"")").stringValue() == "say \"hi\"");
    CHECK(parseSExpr(R"("back\\slash")").stringValue() == "back\\slash");
    // Parentheses inside a string are content, not structure.
    CHECK(parseSExpr(R"SX("((MPLUS SIMP) 1)")SX").isString());
}

TEST_CASE("bar-quoted symbols keep their name without the bars") {
    CHECK(parseSExpr("|Odd Symbol|").isSymbol("Odd Symbol"));
    CHECK(parseSExpr("(|a b| 1)").at(0).isSymbol("a b"));
}

TEST_CASE("malformed input is rejected rather than half-read") {
    CHECK_THROWS_AS(parseSExpr("(1 2"), mx::ParseError);
    CHECK_THROWS_AS(parseSExpr("1 2)"), mx::ParseError);
    CHECK_THROWS_AS(parseSExpr(""), mx::ParseError);
    CHECK_THROWS_AS(parseSExpr("   "), mx::ParseError);
    CHECK_THROWS_AS(parseSExpr(R"("unterminated)"), mx::ParseError);
    CHECK_THROWS_AS(parseSExpr("|unterminated"), mx::ParseError);
    // Two expressions where one was promised means the frame was mis-split.
    CHECK_THROWS_AS(parseSExpr("1 2"), mx::ParseError);
    CHECK_THROWS_AS(parseSExpr("(1) (2)"), mx::ParseError);
}

TEST_CASE("dotted pairs are rejected explicitly") {
    // Maxima's term representation is proper lists throughout. Treating the dot
    // as an ordinary symbol would corrupt the tree instead of reporting that
    // something unmodelled arrived.
    CHECK_THROWS_AS(parseSExpr("(a . b)"), mx::ParseError);
}

TEST_CASE("reading past the end of a list is an error, not undefined") {
    const SExpr value = parseSExpr("(1 2)");
    CHECK_THROWS_AS(value.at(2), mx::ParseError);
}

TEST_CASE("deep nesting is bounded rather than overflowing the stack") {
    const std::string tooDeep(mx::detail::kMaxSExprDepth + 10, '(');
    CHECK_THROWS_AS(parseSExpr(tooDeep), mx::ParseError);

    // Comfortably deep input still parses.
    const std::size_t depth = 200;
    std::string nested(depth, '(');
    nested += "1";
    nested.append(depth, ')');
    CHECK_NOTHROW(parseSExpr(nested));
}

TEST_CASE("rendering round-trips through the reader") {
    for (const char *text : {
             "42",
             "-7",
             "$X",
             "((MPLUS SIMP) 1 $X)",
             "((MTIMES SIMP) 2 $X ((%SIN SIMP) $X))",
             "((RAT SIMP) 11 15)",
             "((MLIST SIMP))",
             R"("a string")",
             "((BIGFLOAT SIMP 56) 56593902016227522 2)",
         }) {
        CAPTURE(text);
        const SExpr once = parseSExpr(text);
        CHECK(once.toString() == text);
        CHECK(parseSExpr(once.toString()) == once);
    }
}

TEST_CASE("every recorded Maxima reply parses") {
    // tests/golden/internal_forms.tsv, recorded from a real Maxima by
    // tests/golden/record.sh. The point is that the reader is checked against
    // what Maxima actually emits rather than what it was imagined to emit —
    // and that this check needs no Maxima installed.
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
        if (form.empty()) {
            continue;
        }

        CAPTURE(expression);
        CAPTURE(form);
        SExpr parsed;
        REQUIRE_NOTHROW(parsed = parseSExpr(form));
        // Re-rendering must read back identically, which catches a reader that
        // accepts input but quietly loses part of it.
        CHECK(parseSExpr(parsed.toString()) == parsed);
        ++cases;
    }

    // Guard against the file going missing or being silently emptied.
    CHECK(cases >= 35);
}

TEST_CASE("the golden file's own shapes are what the mapping layer expects") {
    // Spot checks on recorded forms, pinning the facts step 9 will rely on.
    SUBCASE("exact rationals stay exact") {
        const SExpr rational = parseSExpr("((RAT SIMP) 11 15)");
        CHECK(rational.at(0).at(0).isSymbol("RAT"));
        CHECK(rational.at(1).asInt64() == 11);
        CHECK(rational.at(2).asInt64() == 15);
    }
    SUBCASE("built-in and user function heads differ only in their sigil") {
        CHECK(parseSExpr("((%SIN SIMP) $X)").at(0).at(0).isSymbol("%SIN"));
        CHECK(parseSExpr("(($F SIMP) $X)").at(0).at(0).isSymbol("$F"));
    }
    SUBCASE("matrices need no special case") {
        const SExpr matrix
            = parseSExpr("(($MATRIX SIMP) ((MLIST SIMP) 1 2) ((MLIST SIMP) 3 4))");
        CHECK(matrix.at(0).at(0).isSymbol("$MATRIX"));
        CHECK(matrix.size() == 3);
    }
    SUBCASE("bigfloats carry precision in the head") {
        const SExpr bigfloat
            = parseSExpr("((BIGFLOAT SIMP 56) 56593902016227522 2)");
        CHECK(bigfloat.at(0).at(0).isSymbol("BIGFLOAT"));
        CHECK(bigfloat.at(0).at(2).asInt64() == 56);
    }
}
