// The canonical order over expressions: the order the normaliser sorts operands
// by, and the one an ordered container of expressions needs.

#include <doctest/doctest.h>

#include "core/normalize.hpp"

#include <proxima/expr.hpp>
#include <proxima/functions.hpp>
#include <proxima/symbol.hpp>

#include <algorithm>
#include <compare>
#include <map>
#include <set>
#include <string>
#include <vector>

using proxima::Expr;
using proxima::Symbol;
using proxima::detail::compare_expr;

TEST_CASE("the order tells apart numbers that a double cannot") {
    // Numbers used to be compared through double, so two integers that round
    // to the same double compared equal while == said they differed. An
    // ordered container keyed on that order would merge them.
    const Expr big(proxima::Integer("1267650600228229401496703205376")); // 2^100
    const Expr bigger(
        proxima::Integer("1267650600228229401496703205377")); // 2^100 + 1
    REQUIRE_FALSE(big == bigger);
    CHECK(compare_expr(big, bigger) < 0);
    CHECK(compare_expr(bigger, big) > 0);

    const Expr third = Expr::rational(
        proxima::Integer("1267650600228229401496703205376"), proxima::Integer(3));
    const Expr third_and_a_bit = Expr::rational(
        proxima::Integer("1267650600228229401496703205377"), proxima::Integer(3));
    REQUIRE_FALSE(third == third_and_a_bit);
    CHECK(compare_expr(third, third_and_a_bit) < 0);
}

TEST_CASE("comparing equal means being equal, across every kind") {
    // What an ordered container relies on: its notion of "the same key" is the
    // order's, and it must agree with ==.
    const Symbol x("x");
    const Symbol y("y");
    const std::vector<Expr> corpus = {
        Expr(0),
        Expr(1),
        Expr(-1),
        Expr(2),
        Expr::rational(1, 2),
        Expr::rational(-1, 2),
        Expr(0.5),
        Expr(2.0),
        Expr(0.0),
        Expr(-0.0),
        Expr(proxima::Integer("1267650600228229401496703205376")),
        Expr(proxima::Integer("1267650600228229401496703205377")),
        Expr(x),
        Expr(y),
        Expr::opaque("\"text\""),
        Expr(x) + 1,
        Expr(x) + 2,
        Expr(x) * Expr(y),
        pow(Expr(x), 2),
        pow(Expr(x), 3),
        proxima::sin(Expr(x)),
        proxima::sin(Expr(y)),
        proxima::cos(Expr(x)),
        eq(Expr(x), Expr(1)),
        lt(Expr(x), Expr(1)),
        eq(Expr(x), Expr(2)),
    };
    for (const Expr &a : corpus) {
        for (const Expr &b : corpus) {
            CAPTURE(a.str());
            CAPTURE(b.str());
            CHECK((compare_expr(a, b) == 0) == (a == b));
            CHECK((compare_expr(a, b) < 0) == (compare_expr(b, a) > 0));
        }
    }
}

TEST_CASE("expressions are keys of ordered containers, under CanonicalLess") {
    // Named explicitly: libc++ 22 ignores a std::less<Expr> specialisation,
    // swapping in std::less<>, which needs an operator< that Expr lacks.
    const Symbol x("x");

    std::set<Expr, proxima::CanonicalLess> seen;
    seen.insert(Expr(x) + 1);
    seen.insert(Expr(1) + Expr(x)); // The same expression.
    seen.insert(Expr(proxima::Integer("1267650600228229401496703205376")));
    seen.insert(Expr(proxima::Integer("1267650600228229401496703205377")));
    CHECK(seen.size() == 3);

    std::map<Expr, int, proxima::CanonicalLess> counts;
    ++counts[proxima::sin(Expr(x))];
    ++counts[proxima::sin(Expr(x))];
    ++counts[proxima::cos(Expr(x))];
    CHECK(counts.size() == 2);
    CHECK(counts[proxima::sin(Expr(x))] == 2);

    const std::set<Symbol, proxima::CanonicalLess> symbols{Symbol("b"), Symbol("a"),
                                                           Symbol("a")};
    CHECK(symbols.size() == 2);
    CHECK(symbols.begin()->name() == "a");

    SUBCASE("and sort canonically: numbers by value, then symbols, then compounds") {
        std::vector<Expr> items{proxima::sin(Expr(x)), Expr(x), Expr(2),
                                Expr::rational(1, 2)};
        std::sort(items.begin(), items.end(), proxima::CanonicalLess{});
        CHECK(items
              == std::vector<Expr>{Expr::rational(1, 2), Expr(2), Expr(x),
                                   proxima::sin(Expr(x))});
    }

    SUBCASE("where 0.0 and -0.0, being equal, are equivalent") {
        CHECK(proxima::canonical_order(Expr(0.0), Expr(-0.0))
              == std::weak_ordering::equivalent);
        CHECK(proxima::canonical_order(Expr(1), Expr(1.0))
              == std::weak_ordering::less);
    }
}
