// Local operations on the expression tree: replace, and contains beside it.
// No kernel anywhere in this file.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <numeric>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <vector>

#include <proxima/errors.hpp>
#include <proxima/expr.hpp>
#include <proxima/functions.hpp>
#include <proxima/numeric.hpp>
#include <proxima/symbol.hpp>
#include <proxima/traverse.hpp>

using proxima::Expr;
using proxima::Symbol;

TEST_CASE("replace rewrites a symbol locally, and the result is normalised") {
    const Symbol x("x");
    const Symbol y("y");

    // Numbers fold and identities go, as for any freshly built expression.
    CHECK(proxima::replace(3 * Expr(x) + 2, x, Expr(2)) == Expr(8));
    CHECK(proxima::replace(Expr(x) * Expr(y), y, Expr(1)) == Expr(x));

    // A symbol can stand for an expression, and relations are rewritten too.
    CHECK(proxima::replace(Expr(x) * Expr(y), x, Expr(y) + 1) == (Expr(y) + 1) * Expr(y));
    CHECK(proxima::replace(eq(Expr(x), Expr(y)), x, Expr(2)) == eq(Expr(2), Expr(y)));

    // Nothing to replace leaves the expression as it was.
    CHECK(proxima::replace(Expr(y) + 1, x, Expr(2)) == Expr(y) + 1);

    SUBCASE("but not evaluated, which is Maxima's work") {
        CHECK(proxima::replace(proxima::sin(Expr(x)), x, Expr(0)) == proxima::sin(Expr(0)));
        CHECK(proxima::replace(pow(Expr(x), 2), x, Expr(2)) == pow(Expr(2), Expr(2)));
    }

    SUBCASE("a function head is a name, not the symbol") {
        CHECK(proxima::replace(Expr::function("x", {Expr(x)}), x, Expr(1))
              == Expr::function("x", {Expr(1)}));
    }

    SUBCASE("unmodelled text that mentions the symbol is refused, not skipped") {
        CHECK_THROWS_AS(proxima::replace(Expr::opaque("matrix([x])") + Expr(x), x, Expr(1)),
                        proxima::Error);
        CHECK(proxima::replace(Expr::opaque("matrix([y])") + Expr(x), x, Expr(1))
              == Expr::opaque("matrix([y])") + 1);
    }

    SUBCASE("and it composes with the numeric layer") {
        // Fix a parameter locally, then compile in the variable.
        const Symbol a("a");
        const Expr f = Expr(a) * pow(Expr(x), 2) + Expr(x);
        const proxima::Compiled fixed(proxima::replace(f, a, Expr(2.5)), x);
        CHECK(fixed(3.0) == doctest::Approx(*proxima::eval_numeric(f, {{"a", 2.5}, {"x", 3.0}})));
    }
}

TEST_CASE("visit goes through every node, a node before its operands") {
    const Symbol x("x");
    // Normalised: the number sorts first, so this is the sum of 1 and sin(x).
    const Expr e = proxima::sin(Expr(x)) + 1;
    std::vector<proxima::Kind> seen;
    proxima::visit(e, [&seen](const Expr &node) { seen.push_back(node.kind()); });
    CHECK(seen == std::vector{proxima::Kind::Add, proxima::Kind::Integer, proxima::Kind::Function,
                              proxima::Kind::Symbol});
}

TEST_CASE("any_of stops at the first node that answers yes") {
    const Expr e = proxima::sin(Expr(Symbol("x"))) + 1;
    int asked = 0;
    CHECK(proxima::any_of(e, [&asked](const Expr &node) {
        ++asked;
        return node.is(proxima::Kind::Integer);
    }));
    CHECK(asked == 2); // The sum, then its first operand.

    CHECK_FALSE(proxima::any_of(e, [](const Expr &node) { return node.is(proxima::Kind::Real); }));
}

TEST_CASE("transform rewrites bottom-up and shares what it leaves alone") {
    const Symbol x("x");
    const Symbol y("y");

    SUBCASE("each node once, operands first, parents rebuilt and normalised") {
        const Expr e = 2 * Expr(x) + 3;
        int calls = 0;
        const Expr doubled = proxima::transform(e, [&calls](const Expr &node) -> Expr {
            ++calls;
            return node.is(proxima::Kind::Integer) ? Expr(node.integer_value()) * 2 : node;
        });
        CHECK(doubled == 4 * Expr(x) + 6);
        CHECK(calls == 5); // 3, then 2 and x, then 2*x, then the sum.
    }

    SUBCASE("an untouched subtree is the same representation, not a copy") {
        const Expr untouched = proxima::sin(Expr(y));
        const Expr e = Expr::function("f", {untouched, Expr(x)});
        const Expr result = proxima::replace(e, x, Expr(1));
        CHECK(proxima::detail::same_representation(result.arg(0), untouched));
        CHECK(proxima::detail::same_representation(proxima::replace(e, Symbol("z"), Expr(1)), e));
    }

    SUBCASE("a rewrite to an equal but different value is kept") {
        // 0.0 == -0.0, so a change between them is visible only by identity.
        const Expr e = Expr::function("f", {Expr(-0.0)});
        const Expr result = proxima::transform(e, [](const Expr &node) -> Expr {
            return node.is(proxima::Kind::Real) ? Expr(0.0) : node;
        });
        CHECK_FALSE(std::signbit(result.arg(0).real_value()));
    }

    SUBCASE("and nothing is evaluated") {
        const Expr result = proxima::transform(proxima::sin(Expr(x)), [&x](const Expr &node) -> Expr {
            return node == Expr(x) ? Expr(0) : node;
        });
        CHECK(result == proxima::sin(Expr(0)));
    }
}

TEST_CASE("contains is reachable from its own header") {
    // It moved here from proxima/ops.hpp, which still includes this header.
    const Symbol x("x");
    CHECK(proxima::contains(proxima::sin(Expr(x)) + 1, x));
    CHECK_FALSE(proxima::contains(proxima::sin(Expr(Symbol("y"))), x));
}

// --- nodes, fold and rewrite ----------------------------------------------------

using proxima::Kind;

static_assert(std::ranges::input_range<proxima::Nodes>);

TEST_CASE("nodes is the tree as a range, in visit's order") {
    const Symbol x("x");
    const Expr e = proxima::sin(Expr(x)) + pow(Expr(x), 2);

    std::vector<std::string> visited;
    proxima::visit(e, [&](const Expr &n) { visited.push_back(n.str()); });
    std::vector<std::string> ranged;
    for (const Expr &n : proxima::nodes(e)) {
        ranged.push_back(n.str());
    }
    CHECK(ranged == visited);

    SUBCASE("and the standard algorithms apply") {
        CHECK(std::ranges::distance(proxima::nodes(e)) == 6);
        CHECK(std::ranges::any_of(proxima::nodes(e),
                                  [](const Expr &n) { return n.is(Kind::Function); }));
        CHECK(std::ranges::count_if(proxima::nodes(e),
                                    [&](const Expr &n) { return n == Expr(x); })
              == 2);
        auto numbers = proxima::nodes(e)
                       | std::views::filter([](const Expr &n) { return n.is_number(); });
        CHECK(std::ranges::distance(numbers) == 1);
    }
    SUBCASE("a temporary is safe to walk: the range keeps its own copy") {
        std::size_t count = 0;
        for ([[maybe_unused]] const Expr &n : proxima::nodes(Expr(x) + 1)) {
            ++count;
        }
        CHECK(count == 3);
    }
    SUBCASE("an iterator can be moved mid-walk") {
        auto it = proxima::nodes(e).begin();
        ++it;
        auto moved = std::move(it);
        ++moved;
        CHECK(moved->str() == "x");
    }
    SUBCASE("a leaf is a one-node range") {
        CHECK(std::ranges::distance(proxima::nodes(Expr(x))) == 1);
    }
}

TEST_CASE("fold is the recursion the other walks are cases of") {
    const Symbol x("x");
    const Symbol y("y");
    const Expr e = proxima::sin(Expr(x) * y) + pow(Expr(x), 2) + 1;

    const auto size = proxima::fold<std::size_t>(
        e, [](const Expr &, std::span<const std::size_t> sizes) {
            return std::accumulate(sizes.begin(), sizes.end(), std::size_t{1});
        });
    CHECK(size == static_cast<std::size_t>(std::ranges::distance(proxima::nodes(e))));

    const auto depth = proxima::fold<int>(e, [](const Expr &, std::span<const int> depths) {
        return 1 + (depths.empty() ? 0 : *std::ranges::max_element(depths));
    });
    CHECK(depth == 4); // sum > sin > product > x

    SUBCASE("to bool, which a vector cannot hold as a span") {
        const auto mentions_y = [&](const Expr &tree) {
            return proxima::fold<bool>(tree, [&](const Expr &n, std::span<const bool> below) {
                return n == Expr(y) || std::ranges::any_of(below, [](bool b) { return b; });
            });
        };
        CHECK(mentions_y(e));
        CHECK_FALSE(mentions_y(pow(Expr(x), 2)));
    }
    SUBCASE("with match telling it the kind") {
        const auto calls = proxima::fold<int>(e, [](const Expr &n, std::span<const int> below) {
            const int here = n.match([](const proxima::node::Call &) { return 1; },
                                     [](const auto &) { return 0; });
            return here + std::accumulate(below.begin(), below.end(), 0);
        });
        CHECK(calls == 1);
    }
}

TEST_CASE("rewrite replaces what f says to, and shares the rest") {
    const Symbol x("x");
    const Symbol y("y");
    const auto sin_to_cos = [](const Expr &n) -> std::optional<Expr> {
        if (n.is(Kind::Function) && n.name() == "sin") {
            return proxima::cos(n.arg(0));
        }
        return std::nullopt;
    };

    const Expr untouched = pow(Expr(y), 3) * proxima::log(Expr(y));
    const Expr e = proxima::sin(Expr(x)) + untouched;
    const Expr rewritten = proxima::rewrite(e, sin_to_cos);
    CHECK(rewritten == proxima::cos(Expr(x)) + untouched);

    // The part f never touched is the same representation, not a copy.
    const auto shared = std::ranges::any_of(rewritten.args(), [&](const Expr &term) {
        return proxima::detail::same_representation(term, e.args()[1])
               || proxima::detail::same_representation(term, e.args()[0]);
    });
    CHECK(shared);

    SUBCASE("changing nothing hands back the very same expression") {
        const Expr same = proxima::rewrite(e, [](const Expr &) { return std::optional<Expr>(); });
        CHECK(proxima::detail::same_representation(same, e));
    }
    SUBCASE("bottom-up, and normalised as it goes") {
        // Every x becomes 2 first; the sum then folds as a new one would.
        const Expr folded = proxima::rewrite(3 * Expr(x) + 2, [&](const Expr &n) -> std::optional<Expr> {
            if (n == Expr(x)) {
                return Expr(2);
            }
            return std::nullopt;
        });
        CHECK(folded == Expr(8));
    }
}
