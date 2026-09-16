// The reply cache: the LRU on its own, then the rules about when a cached
// answer stops being true, which is the part that has to be right.

#include <doctest/doctest.h>

#include "kernel/cache.hpp"

#include <proxima/context.hpp>
#include <proxima/expr.hpp>
#include <proxima/functions.hpp>
#include <proxima/kernel.hpp>
#include <proxima/ops.hpp>
#include <proxima/symbol.hpp>

#include <chrono>
#include <string>

using proxima::Expr;
using proxima::Symbol;
using proxima::detail::ReplyCache;

namespace {

proxima::Reply valued(const std::string &value) {
    proxima::Reply reply;
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
    const proxima::Reply *found = cache.find("a");
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

TEST_CASE("the byte limit evicts as well as the entry limit") {
    // Room for many entries but few bytes: large replies must push out old
    // ones long before the count would.
    const std::string large(1000, 'x');
    const std::size_t each = ReplyCache::footprint("a", valued(large));
    ReplyCache cache(4096, 2 * each + each / 2);

    cache.insert("a", valued(large));
    cache.insert("b", valued(large));
    CHECK(cache.size() == 2);
    CHECK(cache.bytes() == 2 * each);

    cache.insert("c", valued(large));
    CHECK(cache.size() == 2);
    CHECK(cache.bytes() <= cache.byteLimit());
    CHECK(cache.find("a") == nullptr);
    CHECK(cache.find("b") != nullptr);
    CHECK(cache.find("c") != nullptr);
}

TEST_CASE("a reply larger than the byte limit is not remembered") {
    ReplyCache cache(16, 2048);
    cache.insert("small", valued("1"));
    cache.insert("big", valued(std::string(4096, 'x')));

    CHECK(cache.find("big") == nullptr);
    // And it did not evict what was there to make room it could never use.
    CHECK(cache.find("small") != nullptr);

    // A newer answer too large to keep takes the older one with it, so a
    // stale reply is never served in place of the one that replaced it.
    cache.insert("small", valued(std::string(4096, 'y')));
    CHECK(cache.find("small") == nullptr);
    CHECK(cache.bytes() == 0);
}

TEST_CASE("replacing an entry recharges its footprint") {
    ReplyCache cache(4);
    cache.insert("a", valued(std::string(100, 'x')));
    cache.insert("a", valued("1"));
    CHECK(cache.bytes() == ReplyCache::footprint("a", valued("1")));

    cache.clear();
    CHECK(cache.bytes() == 0);
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
    proxima::Kernel kernel;
    const Symbol x("x");

    const Expr first = proxima::diff(pow(Expr(x), 10), x, 1, kernel);
    const auto afterFirst = kernel.cacheStats();

    const Expr second = proxima::diff(pow(Expr(x), 10), x, 1, kernel);
    const auto afterSecond = kernel.cacheStats();

    CHECK(first == second);
    CHECK(afterSecond.hits == afterFirst.hits + 1);
    CHECK(afterSecond.misses == afterFirst.misses);
}

TEST_CASE("a failure is cached too") {
    // "Maxima cannot integrate this" is as stable an answer as any other, and
    // re-asking costs the same round trip.
    proxima::Kernel kernel;
    const Symbol x("x");

    REQUIRE_FALSE(proxima::integrate(proxima::exp(proxima::sin(Expr(x))), x, kernel).has_value());
    const auto before = kernel.cacheStats();
    REQUIRE_FALSE(proxima::integrate(proxima::exp(proxima::sin(Expr(x))), x, kernel).has_value());

    CHECK(kernel.cacheStats().hits == before.hits + 1);
}

TEST_CASE("an assumption invalidates answers computed without it") {
    // The correctness case for the whole mechanism. sqrt(x^2) is abs(x) until
    // x > 0 is assumed, and x afterwards. A cache that survived the assumption
    // would keep handing back abs(x) — confidently, and wrongly.
    proxima::Kernel kernel;
    const Symbol x("cache_assumption_probe");
    const Expr root = proxima::sqrt(pow(Expr(x), 2));

    CHECK(proxima::simplify(root, kernel) == proxima::abs(Expr(x)));

    {
        proxima::Context ctx(kernel);
        ctx.assume(gt(Expr(x), Expr(0)));
        CHECK(proxima::simplify(root, kernel) == Expr(x));
    }

    // And leaving the scope invalidates just as much as entering it did.
    CHECK(proxima::simplify(root, kernel) == proxima::abs(Expr(x)));
}

TEST_CASE("a raw eval discards the cache, since it could have changed anything") {
    proxima::Kernel kernel;
    const Symbol x("x");

    proxima::diff(pow(Expr(x), 3), x, 1, kernel);
    REQUIRE(kernel.cacheStats().entries > 0);

    kernel.eval("2 + 2");
    CHECK(kernel.cacheStats().entries == 0);
}

TEST_CASE("a binding made through eval cannot leave a stale answer behind") {
    // The hazard the blunt invalidation above exists for: nothing in the text
    // of "cache_binding_probe: 7" says it is an instruction rather than a
    // question, so eval assumes the worst.
    proxima::Kernel kernel;
    const Symbol x("x");

    REQUIRE(kernel.eval("cache_binding_probe: 2").ok);
    const Expr before
        = proxima::simplify(Expr::symbol("cache_binding_probe") * Expr(x), kernel);
    CHECK(before == 2 * Expr(x));

    REQUIRE(kernel.eval("cache_binding_probe: 3").ok);
    const Expr after
        = proxima::simplify(Expr::symbol("cache_binding_probe") * Expr(x), kernel);
    CHECK(after == 3 * Expr(x));
}

TEST_CASE("caching can be turned off") {
    proxima::Config config;
    config.cacheEntries = 0;

    proxima::Kernel kernel(config);
    const Symbol x("x");

    proxima::diff(pow(Expr(x), 4), x, 1, kernel);
    proxima::diff(pow(Expr(x), 4), x, 1, kernel);

    CHECK(kernel.cacheStats().entries == 0);
    CHECK(kernel.cacheStats().hits == 0);
}

TEST_CASE("a cached answer is worth having") {
    // Not a benchmark, just evidence that the round trip is the expensive part
    // and that skipping it is worth the bookkeeping.
    proxima::Kernel kernel;
    const Symbol x("x");
    const Expr heavy = proxima::expand(pow(Expr(x) + 1, 40), kernel);

    const auto timed = [&](auto &&work) {
        const auto start = std::chrono::steady_clock::now();
        work();
        return std::chrono::steady_clock::now() - start;
    };

    const auto cold = timed([&] { proxima::factor(heavy, kernel); });
    const auto warm = timed([&] { proxima::factor(heavy, kernel); });

    CHECK(warm < cold);
}

} // TEST_SUITE("maxima")
