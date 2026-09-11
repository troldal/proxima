// The reply cache: the LRU on its own, then the rules about when a cached
// answer stops being true, which is the part that has to be right.

#include <doctest/doctest.h>

#include "kernel/cache.hpp"

#include <mx/context.hpp>
#include <mx/expr.hpp>
#include <mx/functions.hpp>
#include <mx/kernel.hpp>
#include <mx/ops.hpp>
#include <mx/symbol.hpp>

#include <chrono>
#include <string>

using mx::Expr;
using mx::Symbol;
using mx::detail::ReplyCache;

namespace {

mx::Reply valued(const std::string &value) {
    mx::Reply reply;
    reply.ok = true;
    reply.value = value;
    return reply;
}

} // namespace

// --- Maxima-free ----------------------------------------------------------

TEST_CASE("a cache returns what it was given") {
    ReplyCache cache(4);
    CHECK(cache.find("a") == nullptr);
    CHECK(cache.misses() == 1);

    cache.insert("a", valued("1"));
    const mx::Reply *found = cache.find("a");
    REQUIRE(found != nullptr);
    CHECK(found->value == "1");
    CHECK(cache.hits() == 1);
}

TEST_CASE("the least recently used entry is the one evicted") {
    ReplyCache cache(2);
    cache.insert("a", valued("1"));
    cache.insert("b", valued("2"));

    // Touching "a" makes "b" the oldest.
    REQUIRE(cache.find("a") != nullptr);
    cache.insert("c", valued("3"));

    CHECK(cache.size() == 2);
    CHECK(cache.find("a") != nullptr);
    CHECK(cache.find("c") != nullptr);
    CHECK(cache.find("b") == nullptr);
}

TEST_CASE("re-inserting a key replaces rather than duplicates") {
    ReplyCache cache(4);
    cache.insert("a", valued("1"));
    cache.insert("a", valued("2"));

    CHECK(cache.size() == 1);
    CHECK(cache.find("a")->value == "2");
}

TEST_CASE("a capacity of zero disables caching without misbehaving") {
    ReplyCache cache(0);
    cache.insert("a", valued("1"));
    CHECK(cache.size() == 0);
    CHECK(cache.find("a") == nullptr);
}

TEST_CASE("clearing forgets everything but keeps the counters") {
    ReplyCache cache(4);
    cache.insert("a", valued("1"));
    REQUIRE(cache.find("a") != nullptr);

    cache.clear();
    CHECK(cache.size() == 0);
    CHECK(cache.find("a") == nullptr);
    CHECK(cache.hits() == 1); // The earlier hit still happened.
}

// --- Against a real kernel -------------------------------------------------

TEST_SUITE("maxima") {

TEST_CASE("an answer already given is not asked for again") {
    mx::Kernel kernel;
    const Symbol x("x");

    const Expr first = mx::diff(pow(Expr(x), 10), x, 1, kernel);
    const auto afterFirst = kernel.cacheStats();

    const Expr second = mx::diff(pow(Expr(x), 10), x, 1, kernel);
    const auto afterSecond = kernel.cacheStats();

    CHECK(first == second);
    CHECK(afterSecond.hits == afterFirst.hits + 1);
    CHECK(afterSecond.misses == afterFirst.misses);
}

TEST_CASE("a failure is cached too") {
    // "Maxima cannot integrate this" is as stable an answer as any other, and
    // re-asking costs the same round trip.
    mx::Kernel kernel;
    const Symbol x("x");

    REQUIRE_FALSE(mx::integrate(mx::exp(mx::sin(Expr(x))), x, kernel).has_value());
    const auto before = kernel.cacheStats();
    REQUIRE_FALSE(mx::integrate(mx::exp(mx::sin(Expr(x))), x, kernel).has_value());

    CHECK(kernel.cacheStats().hits == before.hits + 1);
}

TEST_CASE("an assumption invalidates answers computed without it") {
    // The correctness case for the whole mechanism. sqrt(x^2) is abs(x) until
    // x > 0 is assumed, and x afterwards. A cache that survived the assumption
    // would keep handing back abs(x) — confidently, and wrongly.
    mx::Kernel kernel;
    const Symbol x("cache_assumption_probe");
    const Expr root = mx::sqrt(pow(Expr(x), 2));

    CHECK(mx::simplify(root, kernel) == mx::abs(Expr(x)));

    {
        mx::Context ctx(kernel);
        ctx.assume(gt(Expr(x), Expr(0)));
        CHECK(mx::simplify(root, kernel) == Expr(x));
    }

    // And leaving the scope invalidates just as much as entering it did.
    CHECK(mx::simplify(root, kernel) == mx::abs(Expr(x)));
}

TEST_CASE("a raw eval discards the cache, since it could have changed anything") {
    mx::Kernel kernel;
    const Symbol x("x");

    mx::diff(pow(Expr(x), 3), x, 1, kernel);
    REQUIRE(kernel.cacheStats().entries > 0);

    kernel.eval("2 + 2");
    CHECK(kernel.cacheStats().entries == 0);
}

TEST_CASE("a binding made through eval cannot leave a stale answer behind") {
    // The hazard the blunt invalidation above exists for: nothing in the text
    // of "cache_binding_probe: 7" says it is an instruction rather than a
    // question, so eval assumes the worst.
    mx::Kernel kernel;
    const Symbol x("x");

    REQUIRE(kernel.eval("cache_binding_probe: 2").ok);
    const Expr before
        = mx::simplify(Expr::symbol("cache_binding_probe") * Expr(x), kernel);
    CHECK(before == 2 * Expr(x));

    REQUIRE(kernel.eval("cache_binding_probe: 3").ok);
    const Expr after
        = mx::simplify(Expr::symbol("cache_binding_probe") * Expr(x), kernel);
    CHECK(after == 3 * Expr(x));
}

TEST_CASE("caching can be turned off") {
    mx::Config config;
    config.cacheEntries = 0;

    mx::Kernel kernel(config);
    const Symbol x("x");

    mx::diff(pow(Expr(x), 4), x, 1, kernel);
    mx::diff(pow(Expr(x), 4), x, 1, kernel);

    CHECK(kernel.cacheStats().entries == 0);
    CHECK(kernel.cacheStats().hits == 0);
}

TEST_CASE("a cached answer is worth having") {
    // Not a benchmark, just evidence that the round trip is the expensive part
    // and that skipping it is worth the bookkeeping.
    mx::Kernel kernel;
    const Symbol x("x");
    const Expr heavy = mx::expand(pow(Expr(x) + 1, 40), kernel);

    const auto timed = [&](auto &&work) {
        const auto start = std::chrono::steady_clock::now();
        work();
        return std::chrono::steady_clock::now() - start;
    };

    const auto cold = timed([&] { mx::factor(heavy, kernel); });
    const auto warm = timed([&] { mx::factor(heavy, kernel); });

    CHECK(warm < cold);
}

} // TEST_SUITE("maxima")
