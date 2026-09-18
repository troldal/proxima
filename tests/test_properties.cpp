// Property tests: the invariants the design rests on, checked on generated
// expressions rather than hand-picked ones. No kernel.
//
// Each property runs on kDefaultCases random trees from a fixed seed, so a run
// is reproducible. Two environment variables widen it:
//
//     PROXIMA_PROPERTY_CASES   how many trees per property (default 400)
//     PROXIMA_PROPERTY_SEED    the seed (default fixed; "random" for a fresh one)
//
// A failure reports the seed, the case number and the expression.

#include <doctest/doctest.h>

#include "kernel/discovery.hpp"
#include "wire/from_maxima.hpp"
#include "wire/sexpr.hpp"
#include "wire/to_maxima.hpp"

#include <proxima/errors.hpp>
#include <proxima/expr.hpp>
#include <proxima/integer.hpp>
#include <proxima/mathml.hpp>
#include <proxima/symbol.hpp>
#include <proxima/tex.hpp>
#include <proxima/traverse.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <compare>
#include <cstdlib>
#include <format>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

using proxima::Expr;
using proxima::Integer;
using proxima::Kind;
using proxima::RelOp;

namespace {

constexpr int kDefaultCases = 400;
constexpr std::uint64_t kDefaultSeed = 0x5eed'ca5e'2026'0918;

/// The library's own environment reader: std::getenv is deprecated under MSVC
/// and reads through the ANSI code page on Windows.
std::optional<std::string> environment(const char *name) {
    return proxima::detail::system_env()(name);
}

int case_count() {
    if (const auto text = environment("PROXIMA_PROPERTY_CASES")) {
        const long cases = std::strtol(text->c_str(), nullptr, 10);
        if (cases > 0 && cases <= 10'000'000) {
            return static_cast<int>(cases);
        }
    }
    return kDefaultCases;
}

std::uint64_t seed() {
    static const std::uint64_t chosen = [] {
        const auto text = environment("PROXIMA_PROPERTY_SEED");
        if (!text) {
            return kDefaultSeed;
        }
        if (*text == "random") {
            std::random_device device;
            return (std::uint64_t{device()} << 32) ^ device();
        }
        return static_cast<std::uint64_t>(std::strtoull(text->c_str(), nullptr, 0));
    }();
    return chosen;
}

/// What the generator may produce. The text round trip needs the subset the
/// infix printer and parser share; the wire needs everything but Opaque.
struct Shape {
    bool opaque = false;       ///< Opaque nodes, which neither round trip models.
    bool odd_names = false;    ///< Symbol names only the wire can carry: "X", "xY", "x y".
};

/// Random expression trees, built through the public builders, so every tree
/// is already in canonical form — which is what the properties are about.
class Generator {
public:
    Generator(std::uint64_t seed, Shape shape) : rng_(seed), shape_(shape) {}

    Expr expr() {
        // A float overflow in a fold throws; such a tree is not a value, so
        // it is drawn again.
        for (;;) {
            try {
                return node(pick(1, 4));
            } catch (const proxima::Error &) {
            }
        }
    }

private:
    int pick(int low, int high) {
        return std::uniform_int_distribution<int>(low, high)(rng_);
    }
    bool chance(int percent) { return pick(1, 100) <= percent; }

    Expr node(int depth) {
        if (depth <= 0 || chance(25)) {
            return atom();
        }
        switch (pick(0, 8)) {
        case 0:
        case 1:
            return Expr::add(operands(depth, pick(2, 4)));
        case 2:
        case 3:
            return Expr::mul(operands(depth, pick(2, 4)));
        case 4:
            return pow(node(depth - 1), exponent(depth));
        case 5:
            return node(depth - 1) - node(depth - 1);
        case 6:
            return node(depth - 1) / node(depth - 1);
        case 7:
            return function(depth);
        default:
            return chance(30) ? relation(depth) : -node(depth - 1);
        }
    }

    std::vector<Expr> operands(int depth, int count) {
        std::vector<Expr> result;
        result.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            result.push_back(node(depth - 1));
        }
        return result;
    }

    Expr exponent(int depth) {
        switch (pick(0, 3)) {
        case 0:
            return Expr(pick(-3, 4));
        case 1:
            return Expr::rational(pick(-3, 3), pick(2, 5));
        default:
            return node(depth - 1);
        }
    }

    Expr function(int depth) {
        static constexpr std::array<std::string_view, 8> kHeads{
            "sin", "cos", "log", "f", "g", "bessel_j", "list", "factorial"};
        const std::string head(kHeads[static_cast<std::size_t>(pick(0, static_cast<int>(kHeads.size()) - 1))]);
        const int arity = head == "bessel_j" ? 2 : head == "list" ? pick(0, 3)
                                               : head == "f"      ? pick(1, 3)
                                                                  : 1;
        return Expr::function(head, operands(depth, arity));
    }

    Expr relation(int depth) {
        static constexpr std::array<RelOp, 6> kOps{RelOp::Equal,     RelOp::NotEqual,
                                                   RelOp::Less,      RelOp::LessEqual,
                                                   RelOp::Greater,   RelOp::GreaterEqual};
        return Expr::relation(kOps[static_cast<std::size_t>(pick(0, 5))], node(depth - 1),
                              node(depth - 1));
    }

    Expr atom() {
        switch (pick(0, 9)) {
        case 0:
        case 1:
            return Expr(pick(-5, 9));
        case 2:
            return Expr(chance(50) ? big_integer() : near_power_of_two());
        case 3:
            return Expr::rational(pick(-20, 20), pick(1, 12));
        case 4:
            return Expr::real(real());
        case 5:
            if (shape_.opaque && chance(30)) {
                return Expr::opaque(chance(50) ? "\"a string\"" : "?unmodelled");
            }
            [[fallthrough]];
        default:
            return Expr::symbol(symbol_name());
        }
    }

    Integer big_integer() {
        std::string digits(static_cast<std::size_t>(pick(19, 45)), '0');
        digits.front() = static_cast<char>('1' + pick(0, 8));
        for (std::size_t i = 1; i < digits.size(); ++i) {
            digits[i] = static_cast<char>('0' + pick(0, 9));
        }
        return Integer(chance(50) ? "-" + digits : digits);
    }

    /// 2^53, 2^64 or 2^100, give or take two: distinct integers that round to
    /// the same double, which is exactly where an ordering that trusted
    /// doubles once merged different numbers.
    Integer near_power_of_two() {
        static constexpr std::array<int, 3> kPowers{53, 64, 100};
        Integer value(1);
        for (int i = kPowers[static_cast<std::size_t>(pick(0, 2))]; i > 0; --i) {
            value = value * Integer(2);
        }
        value = value + Integer(pick(-2, 2));
        return chance(30) ? -value : value;
    }

    double real() {
        static constexpr std::array<double, 12> kAwkward{
            0.0,   -0.0,   0.1,     2.5,   -7.25,  1.0 / 3.0,
            1e-300, 1e300, 5e-324, 1.7976931348623157e308, 123456789.125, 0.5};
        if (chance(50)) {
            return kAwkward[static_cast<std::size_t>(pick(0, static_cast<int>(kAwkward.size()) - 1))];
        }
        // Any finite double, uniformly over bit patterns.
        for (;;) {
            const std::uint64_t bits = std::uniform_int_distribution<std::uint64_t>()(rng_);
            const double value = std::bit_cast<double>(bits);
            if (std::isfinite(value)) {
                return value;
            }
        }
    }

    std::string symbol_name() {
        static constexpr std::array<std::string_view, 10> kPlain{
            "x", "y", "z", "a_1", "theta", "%pi", "%e", "%i", "inf", "minf"};
        static constexpr std::array<std::string_view, 3> kOdd{"X", "xY", "x y"};
        if (shape_.odd_names && chance(20)) {
            return std::string(kOdd[static_cast<std::size_t>(pick(0, static_cast<int>(kOdd.size()) - 1))]);
        }
        return std::string(kPlain[static_cast<std::size_t>(pick(0, static_cast<int>(kPlain.size()) - 1))]);
    }

    std::mt19937_64 rng_;
    Shape shape_;
};

/// `expr` rebuilt from its parts through the public builders, bottom up.
Expr rebuild(const Expr &expr) {
    std::vector<Expr> parts;
    for (const Expr &operand : expr.args()) {
        parts.push_back(rebuild(operand));
    }
    switch (expr.kind()) {
    case Kind::Integer:
        return Expr::integer(expr.integer_value());
    case Kind::Rational:
        return Expr::rational(expr.numerator(), expr.denominator());
    case Kind::Real:
        return Expr::real(expr.real_value());
    case Kind::Symbol:
        return Expr::symbol(expr.name());
    case Kind::Opaque:
        return Expr::opaque(expr.opaque_text());
    case Kind::Add:
        return Expr::add(std::move(parts));
    case Kind::Mul:
        return Expr::mul(std::move(parts));
    case Kind::Pow:
        return Expr::pow(parts[0], parts[1]);
    case Kind::Function:
        return Expr::function(expr.name(), std::move(parts));
    case Kind::Relation:
        return Expr::relation(expr.relation_op(), parts[0], parts[1]);
    }
    return expr;
}

/// The trees for one property, each with its case number and the seed, for
/// the failure report.
template <typename Check>
void for_all(std::uint64_t salt, Shape shape, Check &&check) {
    const std::uint64_t run_seed = seed() ^ salt;
    Generator generator(run_seed, shape);
    const int cases = case_count();
    for (int i = 0; i < cases; ++i) {
        const Expr expr = generator.expr();
        const std::string printed = expr.str();
        INFO("seed ", run_seed, ", case ", i, ": ", printed);
        check(expr);
    }
}

bool mentions_opaque(const Expr &expr) {
    return proxima::any_of(expr, [](const Expr &node) { return node.is(Kind::Opaque); });
}

} // namespace

TEST_CASE("property: the generator reaches every kind of node") {
    // A property that passes on trees too simple to break it proves nothing.
    // This pins the generator's reach, so a change to it that made the other
    // properties vacuous would fail here instead.
    std::array<int, 10> kinds{};
    int nested_relations = 0;
    int big_integers = 0;
    int negative_zero = 0;
    for_all(0, {.opaque = true, .odd_names = true}, [&](const Expr &expr) {
        proxima::visit(expr, [&](const Expr &node) {
            ++kinds[static_cast<std::size_t>(node.kind())];
            big_integers += node.is(Kind::Integer) && !node.integer_value().is_small() ? 1 : 0;
            negative_zero += node.is(Kind::Real) && node.real_value() == 0.0
                                     && std::signbit(node.real_value())
                                 ? 1
                                 : 0;
            if (!node.is(Kind::Relation)) {
                for (const Expr &operand : node.args()) {
                    nested_relations += operand.is(Kind::Relation) ? 1 : 0;
                }
            }
        });
    });
    for (std::size_t kind = 0; kind < kinds.size(); ++kind) {
        CAPTURE(kind);
        CHECK(kinds[kind] > 0);
    }
    CHECK(nested_relations > 0);
    CHECK(big_integers > 0);
    CHECK(negative_zero > 0);
}

TEST_CASE("property: building an expression from its own parts changes nothing") {
    // Normalisation is idempotent: every tree is canonical when built, so
    // building it again from its canonical parts gives the same tree — equal,
    // and with the same hash.
    for_all(1, {.opaque = true, .odd_names = true}, [](const Expr &expr) {
        const Expr again = rebuild(expr);
        CHECK(again == expr);
        CHECK(again.hash() == expr.hash());
        CHECK(again.str() == expr.str());
    });
}

TEST_CASE("property: equality, ordering and hashing agree") {
    // == is structural, canonical_order is a total order consistent with it,
    // and equal values hash equally. Checked over every pair of a batch that
    // includes rebuilt copies, so equal pairs are not left to chance.
    std::vector<Expr> batch;
    for_all(2, {.opaque = true, .odd_names = true}, [&batch](const Expr &expr) {
        batch.push_back(expr);
        if (batch.size() % 4 == 0) {
            batch.push_back(rebuild(expr));
        }
    });
    batch.resize(std::min<std::size_t>(batch.size(), 300));

    for (std::size_t i = 0; i < batch.size(); ++i) {
        for (std::size_t j = 0; j < batch.size(); ++j) {
            const Expr &a = batch[i];
            const Expr &b = batch[j];
            const auto order = proxima::canonical_order(a, b);
            const auto reverse = proxima::canonical_order(b, a);
            // std::is_eq and friends rather than `order == 0`: doctest would
            // hand the 0 on as an int variable, which MSVC's <compare> refuses.
            const bool equal = a == b;
            if (equal != std::is_eq(order) || (equal && a.hash() != b.hash())
                || std::is_lt(order) != std::is_gt(reverse)) {
                INFO("seed ", seed(), ": ", a.str(), "  vs  ", b.str());
                CHECK(equal == std::is_eq(order));
                CHECK((!equal || a.hash() == b.hash()));
                CHECK(std::is_lt(order) == std::is_gt(reverse));
            }
        }
    }

    // A strict weak ordering sorts, and sorts consistently: once sorted, each
    // element is no greater than every later one, not merely the next.
    std::vector<Expr> sorted = batch;
    std::sort(sorted.begin(), sorted.end(), proxima::CanonicalLess{});
    for (std::size_t i = 0; i < sorted.size(); ++i) {
        for (std::size_t j = i + 1; j < sorted.size(); ++j) {
            if (std::is_gt(proxima::canonical_order(sorted[i], sorted[j]))) {
                INFO(sorted[i].str(), "  sorted before  ", sorted[j].str());
                FAIL("the ordering is not transitive");
            }
        }
    }
}

TEST_CASE("property: printed text parses back to the same expression") {
    // The printer's promise, on trees the fuzzer would rarely reach: deep
    // mixtures of rationals, reals, powers, relations and function calls.
    for_all(3, {}, [](const Expr &expr) {
        const std::string printed = expr.str();
        const auto parsed = Expr::parse(printed);
        if (!parsed) {
            FAIL("refused: ", parsed.error().message());
        }
        CHECK(*parsed == expr);
    });
}

TEST_CASE("property: an expression survives the wire out and back without a kernel") {
    // to_maxima writes the internal form Maxima reads; from_maxima reads the
    // form Maxima writes. Composed, with no Maxima in between, they must be
    // the identity — which is what lets a reply be trusted to mean what was
    // asked. Opaque nodes are left out: they travel as text for Maxima to
    // parse, so only a kernel can bring them back.
    for_all(4, {.odd_names = true}, [](const Expr &expr) {
        const std::string form = proxima::detail::to_maxima(expr);
        INFO("form: ", form);
        Expr back;
        REQUIRE_NOTHROW(back = proxima::detail::from_maxima(proxima::detail::parse_sexpr(form)));
        CHECK(back == expr);
    });
}

TEST_CASE("property: an identity rewrite shares every node") {
    // transform rebuilds only what changed, so rewriting nothing must hand
    // back the very same representation, not an equal copy.
    for_all(5, {.opaque = true, .odd_names = true}, [](const Expr &expr) {
        const Expr same = proxima::transform(expr, [](const Expr &node) { return node; });
        CHECK(proxima::detail::same_representation(same, expr));
    });
}

TEST_CASE("property: contains agrees with a search of the tree") {
    // Outside Opaque text, which it reads more loosely on purpose, contains
    // is exactly "some node is this symbol".
    const std::array<proxima::Symbol, 4> symbols{proxima::Symbol("x"), proxima::Symbol("y"),
                                                 proxima::Symbol("theta"),
                                                 proxima::Symbol("x y")};
    for_all(6, {.odd_names = true}, [&symbols](const Expr &expr) {
        REQUIRE_FALSE(mentions_opaque(expr));
        for (const proxima::Symbol &symbol : symbols) {
            const bool found = proxima::any_of(
                expr, [&symbol](const Expr &node) { return node == symbol.expr(); });
            CHECK(proxima::contains(expr, symbol) == found);
        }
    });
}

TEST_CASE("property: replacing a symbol by itself changes nothing") {
    for_all(7, {.opaque = true, .odd_names = true}, [](const Expr &expr) {
        const proxima::Symbol x("x");
        CHECK(proxima::replace(expr, x, x) == expr);
    });
}

TEST_CASE("property: every renderer accepts every expression") {
    // Rendering is total: TeX, MathML and text never refuse a tree, and
    // std::format's default is the text form.
    for_all(8, {.opaque = true, .odd_names = true}, [](const Expr &expr) {
        CHECK_NOTHROW(static_cast<void>(proxima::to_tex(expr)));
        CHECK_NOTHROW(static_cast<void>(proxima::to_mathml(expr)));
        CHECK(std::format("{}", expr) == expr.str());
    });
}
