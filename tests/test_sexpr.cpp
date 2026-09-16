// S-expression reader tests. Entirely from string literals and the recorded
// golden file — no Maxima, no child process.

#include <doctest/doctest.h>

#include "wire/sexpr.hpp"

#include <proxima/errors.hpp>

#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

using proxima::detail::parse_sexpr;
using proxima::detail::SExpr;

TEST_CASE("atoms") {
    SUBCASE("integers") {
        const SExpr value = parse_sexpr("42");
        REQUIRE(value.is_integer());
        CHECK(value.digits() == "42");
        CHECK(value.as_int64() == 42);
    }
    SUBCASE("negative integers") {
        CHECK(parse_sexpr("-7").as_int64() == -7);
    }
    SUBCASE("symbols") {
        CHECK(parse_sexpr("$X").is_symbol("$X"));
        CHECK(parse_sexpr("MPLUS").is_symbol("MPLUS"));
        CHECK(parse_sexpr("%SIN").is_symbol("%SIN"));
        // Maxima renders true and false as the Lisp booleans, not $TRUE/$FALSE.
        CHECK(parse_sexpr("T").is_symbol("T"));
        CHECK(parse_sexpr("NIL").is_symbol("NIL"));
    }
    SUBCASE("floats") {
        REQUIRE(parse_sexpr("1.5").is_real());
        CHECK(parse_sexpr("1.5").real_value() == doctest::Approx(1.5));
        CHECK(parse_sexpr("1.0e-10").real_value() == doctest::Approx(1.0e-10));
        CHECK(parse_sexpr("-0.25").real_value() == doctest::Approx(-0.25));
    }
    SUBCASE("strings") {
        REQUIRE(parse_sexpr(R"("a string")").is_string());
        CHECK(parse_sexpr(R"("a string")").string_value() == "a string");
    }
}

TEST_CASE("a float is never mistaken for an integer, or the reverse") {
    // The distinction matters downstream: an exact 2 and an inexact 2.0 are
    // different values to a CAS.
    CHECK(parse_sexpr("2").is_integer());
    CHECK(parse_sexpr("2.0").is_real());
    CHECK(parse_sexpr("2.").is_real());
    CHECK(parse_sexpr("2e3").is_real());
}

TEST_CASE("Lisp exponent markers other than e are accepted") {
    // Maxima sets *read-default-float-format* to double-float so `e` is what
    // arrives, but d/s/f/l are legal Common Lisp and would otherwise fail to
    // parse in a thoroughly mystifying way.
    CHECK(parse_sexpr("1.5d0").real_value() == doctest::Approx(1.5));
    CHECK(parse_sexpr("1.5s0").real_value() == doctest::Approx(1.5));
    CHECK(parse_sexpr("2.0d3").real_value() == doctest::Approx(2000.0));
}

TEST_CASE("bignums are kept losslessly rather than truncated") {
    // 30! — Maxima produces these in ordinary use, well past int64. Storing the
    // digits leaves the numeric-representation decision to the layer that has
    // to make it (PLAN.md step 8) instead of silently destroying the value.
    const SExpr value = parse_sexpr("265252859812191058636308480000000");
    REQUIRE(value.is_integer());
    CHECK(value.digits() == "265252859812191058636308480000000");
    CHECK_FALSE(value.as_int64().has_value());
}

TEST_CASE("int64 boundaries are handled exactly") {
    CHECK(parse_sexpr("9223372036854775807").as_int64()
          == std::numeric_limits<std::int64_t>::max());
    CHECK(parse_sexpr("-9223372036854775808").as_int64()
          == std::numeric_limits<std::int64_t>::min());
    // One past the top must report "does not fit", not wrap.
    CHECK_FALSE(parse_sexpr("9223372036854775808").as_int64().has_value());
}

TEST_CASE("lists") {
    SUBCASE("empty") {
        const SExpr value = parse_sexpr("()");
        REQUIRE(value.is_list());
        CHECK(value.empty());
    }
    SUBCASE("flat") {
        const SExpr value = parse_sexpr("(1 2 3)");
        REQUIRE(value.size() == 3);
        CHECK(value.at(0).as_int64() == 1);
        CHECK(value.at(2).as_int64() == 3);
    }
    SUBCASE("nested, as Maxima actually emits") {
        const SExpr value = parse_sexpr("((MPLUS SIMP) 1 $X)");
        REQUIRE(value.size() == 3);

        // The head is itself a list: operator first, then flags. Flags beyond
        // SIMP do occur, so it cannot be treated as a pair.
        const SExpr &head = value.at(0);
        REQUIRE(head.is_list());
        CHECK(head.at(0).is_symbol("MPLUS"));
        CHECK(head.at(1).is_symbol("SIMP"));

        CHECK(value.at(1).as_int64() == 1);
        CHECK(value.at(2).is_symbol("$X"));
    }
    SUBCASE("extra head flags") {
        const SExpr value = parse_sexpr("((MEXPT SIMP RATSIMP) $X 2)");
        CHECK(value.at(0).size() == 3);
    }
}

TEST_CASE("whitespace and newlines between tokens are irrelevant") {
    const SExpr spaced = parse_sexpr("(  (MPLUS\n SIMP)\t1   $X )");
    CHECK(spaced == parse_sexpr("((MPLUS SIMP) 1 $X)"));
}

TEST_CASE("string escapes are undone") {
    CHECK(parse_sexpr(R"("say \"hi\"")").string_value() == "say \"hi\"");
    CHECK(parse_sexpr(R"("back\\slash")").string_value() == "back\\slash");
    // Parentheses inside a string are content, not structure.
    CHECK(parse_sexpr(R"SX("((MPLUS SIMP) 1)")SX").is_string());
}

TEST_CASE("bar-quoted symbols keep their name without the bars") {
    CHECK(parse_sexpr("|Odd Symbol|").is_symbol("Odd Symbol"));
    CHECK(parse_sexpr("(|a b| 1)").at(0).is_symbol("a b"));
}

TEST_CASE("malformed input is rejected rather than half-read") {
    CHECK_THROWS_AS(parse_sexpr("(1 2"), proxima::ParseError);
    CHECK_THROWS_AS(parse_sexpr("1 2)"), proxima::ParseError);
    CHECK_THROWS_AS(parse_sexpr(""), proxima::ParseError);
    CHECK_THROWS_AS(parse_sexpr("   "), proxima::ParseError);
    CHECK_THROWS_AS(parse_sexpr(R"("unterminated)"), proxima::ParseError);
    CHECK_THROWS_AS(parse_sexpr("|unterminated"), proxima::ParseError);
    // Two expressions where one was promised means the frame was mis-split.
    CHECK_THROWS_AS(parse_sexpr("1 2"), proxima::ParseError);
    CHECK_THROWS_AS(parse_sexpr("(1) (2)"), proxima::ParseError);
}

TEST_CASE("a ';' in a reply is refused, rather than read for ever") {
    // Found by fuzzing: ';' ended an atom but nothing consumed it, so the
    // reader produced empty atoms at the same place until memory ran out.
    // Maxima never puts a comment in a reply.
    CHECK_THROWS_AS(static_cast<void>(
                        parse_sexpr("((BIGFLOAT SIMP 56) 450359;96273704960 1)")),
                    proxima::ParseError);
    CHECK_THROWS_AS(static_cast<void>(parse_sexpr(";")), proxima::ParseError);
    CHECK_THROWS_AS(static_cast<void>(parse_sexpr("(a ; b)")), proxima::ParseError);
}

TEST_CASE("dotted pairs are rejected explicitly") {
    // Maxima's term representation is proper lists throughout. Treating the dot
    // as an ordinary symbol would corrupt the tree instead of reporting that
    // something unmodelled arrived.
    CHECK_THROWS_AS(parse_sexpr("(a . b)"), proxima::ParseError);
}

TEST_CASE("a reply truncated by Lisp's print limits is refused, not misread") {
    // Under *print-length* Lisp ends a long list with `...`, and under
    // *print-level* it replaces a deep one with `#`. Both used to read as
    // ordinary symbols, turning a truncated reply into a plausible, wrong
    // expression.
    CHECK_THROWS_AS(parse_sexpr("(1 2 ...)"), proxima::ParseError);
    CHECK_THROWS_AS(parse_sexpr("((MPLUS SIMP) $X #)"), proxima::ParseError);

    SUBCASE("while a symbol really called that is escaped, and still reads") {
        const SExpr quoted = parse_sexpr("(A |...| |#|)");
        REQUIRE(quoted.size() == 3);
        CHECK(quoted.at(1).is_symbol("..."));
        CHECK(quoted.at(2).is_symbol("#"));
    }

    SUBCASE("and an escaped name is never read as a number either") {
        // Lisp bar-quotes a symbol whose name would otherwise read as
        // something else. These used to come back as the integer 123 and the
        // real 1.5.
        const SExpr quoted = parse_sexpr("(|123| |1.5|)");
        REQUIRE(quoted.size() == 2);
        CHECK(quoted.at(0).is_symbol("123"));
        CHECK(quoted.at(1).is_symbol("1.5"));
    }
}

TEST_CASE("reading past the end of a list is an error, not undefined") {
    const SExpr value = parse_sexpr("(1 2)");
    CHECK_THROWS_AS(value.at(2), proxima::ParseError);
}

TEST_CASE("deep nesting is bounded rather than overflowing the stack") {
    const std::string too_deep(proxima::detail::kMaxSExprDepth + 10, '(');
    CHECK_THROWS_AS(parse_sexpr(too_deep), proxima::ParseError);

    // Comfortably deep input still parses.
    const std::size_t depth = 200;
    std::string nested(depth, '(');
    nested += "1";
    nested.append(depth, ')');
    CHECK_NOTHROW(parse_sexpr(nested));
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
        const SExpr once = parse_sexpr(text);
        CHECK(once.to_string() == text);
        CHECK(parse_sexpr(once.to_string()) == once);
    }
}

TEST_CASE("every recorded Maxima reply parses") {
    // tests/golden/internal_forms.tsv, recorded from a real Maxima by
    // tests/golden/record.sh. The point is that the reader is checked against
    // what Maxima actually emits rather than what it was imagined to emit —
    // and that this check needs no Maxima installed.
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
        if (form.empty()) {
            continue;
        }

        CAPTURE(expression);
        CAPTURE(form);
        SExpr parsed;
        REQUIRE_NOTHROW(parsed = parse_sexpr(form));
        // Re-rendering must read back identically, which catches a reader that
        // accepts input but quietly loses part of it.
        CHECK(parse_sexpr(parsed.to_string()) == parsed);
        ++cases;
    }

    // Guard against the file going missing or being silently emptied.
    CHECK(cases >= 35);
}

TEST_CASE("the golden file's own shapes are what the mapping layer expects") {
    // Spot checks on recorded forms, pinning the facts step 9 will rely on.
    SUBCASE("exact rationals stay exact") {
        const SExpr rational = parse_sexpr("((RAT SIMP) 11 15)");
        CHECK(rational.at(0).at(0).is_symbol("RAT"));
        CHECK(rational.at(1).as_int64() == 11);
        CHECK(rational.at(2).as_int64() == 15);
    }
    SUBCASE("built-in and user function heads differ only in their sigil") {
        CHECK(parse_sexpr("((%SIN SIMP) $X)").at(0).at(0).is_symbol("%SIN"));
        CHECK(parse_sexpr("(($F SIMP) $X)").at(0).at(0).is_symbol("$F"));
    }
    SUBCASE("matrices need no special case") {
        const SExpr matrix
            = parse_sexpr("(($MATRIX SIMP) ((MLIST SIMP) 1 2) ((MLIST SIMP) 3 4))");
        CHECK(matrix.at(0).at(0).is_symbol("$MATRIX"));
        CHECK(matrix.size() == 3);
    }
    SUBCASE("bigfloats carry precision in the head") {
        const SExpr bigfloat
            = parse_sexpr("((BIGFLOAT SIMP 56) 56593902016227522 2)");
        CHECK(bigfloat.at(0).at(0).is_symbol("BIGFLOAT"));
        CHECK(bigfloat.at(0).at(2).as_int64() == 56);
    }
}
