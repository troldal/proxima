// Assumptions: a value passed to each operation, not a scope held open in the
// kernel. The value's own rules need no Maxima; what Maxima makes of it does.

#include <doctest/doctest.h>

#include <proxima/assumptions.hpp>
#include <proxima/errors.hpp>
#include <proxima/expr.hpp>
#include <proxima/functions.hpp>
#include <proxima/kernel.hpp>
#include <proxima/ops.hpp>
#include <proxima/symbol.hpp>

#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

using proxima::Assumptions;
using proxima::Expr;
using proxima::Feature;
using proxima::Symbol;
using proxima::Truth;

// --- the value -------------------------------------------------------------------

TEST_CASE("assumptions are a value: equal when they say the same, in any order") {
    const Symbol x("x");
    const Symbol n("n");

    const Assumptions one
        = proxima::assuming(gt(x, 0)).with(n, Feature::Integer).with(lt(x, 1));
    const Assumptions two
        = proxima::declaring(n, Feature::Integer).with(lt(x, 1)).with(gt(x, 0));
    CHECK(one == two);
    CHECK(one.hash() == two.hash());
    CHECK(std::hash<Assumptions>{}(one) == std::hash<Assumptions>{}(two));

    // Saying a thing twice says it once.
    CHECK(one.with(gt(x, 0)) == one);
    CHECK(one.facts().size() == 2);
    CHECK(one.declarations().size() == 1);

    // And adding makes a new value, leaving the old one alone.
    const Assumptions more = one.with(gt(n, 0));
    CHECK(more != one);
    CHECK(one.facts().size() == 2);

    CHECK(Assumptions().empty());
    CHECK_FALSE(one.empty());
    CHECK(proxima::assuming({gt(x, 0), lt(x, 1)})
          == proxima::assuming(lt(x, 1)).with(gt(x, 0)));
    CHECK(one.with(proxima::assuming(gt(n, 0))) == more);

    const std::unordered_set<Assumptions> seen{one, two, more};
    CHECK(seen.size() == 2);
}

TEST_CASE("every Feature has Maxima's name") {
    CHECK(proxima::name_of(Feature::Integer) == "integer");
    CHECK(proxima::name_of(Feature::OddFun) == "oddfun");
    CHECK(proxima::name_of(Feature::AntiSymmetric) == "antisymmetric");
}

TEST_SUITE("maxima") {

// --- what Maxima makes of them
// -----------------------------------------------------

TEST_CASE("an assumption changes what Maxima can conclude, for that call alone") {
    const Symbol x("x");
    const Expr root = proxima::sqrt(pow(x, 2));

    // Without knowing the sign, the best Maxima can do is |x|.
    CHECK(*proxima::simplify(root) == proxima::abs(Expr(x)));
    // Asked under x > 0, the same question has another answer...
    CHECK(*proxima::simplify(root, proxima::assuming(gt(x, 0))) == Expr(x));
    // ...and nothing is left behind: the next call is asked under nothing.
    CHECK(*proxima::simplify(root) == proxima::abs(Expr(x)));
}

TEST_CASE("a declaration is part of the value too") {
    const Symbol n("n");
    proxima::Kernel &kernel = proxima::shared_kernel();

    CHECK(kernel.ask(proxima::Query::text("featurep(n, integer)"),
                     proxima::declaring(n, Feature::Integer))
          == Expr::symbol("true"));
    CHECK(kernel.ask(proxima::Query::text("featurep(n, integer)"))
          == Expr::symbol("false"));
}

TEST_CASE("Maxima accepts every Feature") {
    // Feature::Prime existed, was documented, and made declare throw: Maxima
    // has no such feature. Each on a symbol of its own, so that opposites
    // (even and odd) cannot contradict one another.
    const auto last = static_cast<int>(Feature::AntiSymmetric);
    proxima::Kernel &kernel = proxima::shared_kernel();
    for (int i = 0; i <= last; ++i) {
        const auto feature = static_cast<Feature>(i);
        const std::string name(proxima::name_of(feature));
        CAPTURE(name);
        const Symbol s("feature_probe_" + name);
        const auto answer = kernel.ask(
            proxima::Query::text("featurep(" + s.name() + ", " + name + ")"),
            proxima::declaring(s, feature));
        REQUIRE(answer.has_value());
        CHECK(*answer == Expr::symbol("true"));
    }
}

TEST_CASE("is answers under the assumptions it is given") {
    const Symbol a("is_probe_a");
    const auto positive = proxima::assuming(gt(a, 0));
    CHECK(proxima::is(gt(a, 0), positive) == Truth::True);
    CHECK(proxima::is(lt(a, 0), positive) == Truth::False);
    CHECK(proxima::is(gt(a, 0)) == Truth::Unknown);
}

TEST_CASE("several facts, and a declaration, together") {
    const Symbol x("together_x");
    const Symbol k("together_k");
    const auto both
        = proxima::assuming({gt(x, 0), gt(k, 1)}).with(k, Feature::Integer);
    CHECK(proxima::is(gt(x * k, 0), both) == Truth::True);
    CHECK(proxima::shared_kernel().ask(
              proxima::Query::text("featurep(together_k, integer)"), both)
          == Expr::symbol("true"));
}

TEST_CASE("contradictory assumptions are refused, with their own cause") {
    const Symbol x("contradiction_probe");
    const auto both = proxima::assuming({gt(x, 0), lt(x, 0)});

    const auto refused = proxima::simplify(proxima::sqrt(pow(x, 2)), both);
    REQUIRE_FALSE(refused.has_value());
    CHECK(proxima::cause_of(refused.error()) == proxima::Cause::Inconsistent);
    CHECK(refused.error().message().find("contradiction_probe")
          != std::string::npos);

    // And the kernel is none the worse: the next call, under consistent
    // assumptions or none, is answered.
    CHECK(proxima::is(gt(x, 0), proxima::assuming(gt(x, 0))) == Truth::True);
    CHECK(proxima::is(gt(x, 0)) == Truth::Unknown);
}

TEST_CASE("a redundant assumption is accepted quietly") {
    const Symbol x("redundant_probe");
    CHECK(proxima::is(gt(x, 0), proxima::assuming({gt(x, 0), gt(x, -1)}))
          == Truth::True);
}

TEST_CASE("answers are cached under their assumptions, not across them") {
    proxima::Kernel kernel;
    const Symbol x("x");
    const Expr root = proxima::sqrt(pow(x, 2));
    const auto positive = proxima::assuming(gt(x, 0));

    CHECK(*proxima::simplify(root, kernel) == proxima::abs(Expr(x)));
    CHECK(*proxima::simplify(root, {positive, kernel}) == Expr(x));
    const auto misses = kernel.cache_stats().misses;

    // Each asked again: from the cache, and each the answer to its own
    // question — the positive one does not leak into the bare one.
    CHECK(*proxima::simplify(root, {positive, kernel}) == Expr(x));
    CHECK(*proxima::simplify(root, kernel) == proxima::abs(Expr(x)));
    CHECK(kernel.cache_stats().misses == misses);
    CHECK(kernel.cache_stats().hits >= 2);
}

TEST_CASE("threads asking under different assumptions each get their own answer") {
    // What a scope held open in the kernel could not promise: one thread's
    // assumptions were every thread's. Now they travel with each question.
    proxima::Kernel kernel;
    const Symbol x("thread_assumption_x");
    const Expr root = proxima::sqrt(pow(x, 2));
    const auto positive = proxima::assuming(gt(x, 0));
    const auto negative = proxima::assuming(lt(x, 0));

    int wrong = 0;
    std::thread plus([&] {
        for (int i = 0; i < 20; ++i) {
            wrong += proxima::simplify(root, {positive, kernel}) == Expr(x) ? 0 : 1;
        }
    });
    std::thread minus([&] {
        for (int i = 0; i < 20; ++i) {
            wrong += proxima::simplify(root, {negative, kernel}) == -Expr(x) ? 0 : 1;
        }
    });
    plus.join();
    minus.join();
    CHECK(wrong == 0);
}

// --- the hazard assumptions answer
// -------------------------------------------------

TEST_CASE("a question Maxima would ask becomes a failure, not a deadlock") {
    // integrate(x^n, x) cannot proceed without knowing whether n is -1, and
    // Maxima's way of finding out is to print a prompt and read a line. Over a
    // pipe that blocks until the timeout and then swallows the *next* request
    // as the answer, desynchronising every reply after it.
    //
    // Overriding Maxima's `retrieve` turns the question into an ordinary error.
    const Symbol x("x");
    const Symbol n("question_probe_n");

    const auto ambiguous = proxima::integrate(pow(x, Expr(n)), x);
    REQUIRE_FALSE(ambiguous.has_value());
    CHECK(proxima::cause_of(ambiguous.error()) == proxima::Cause::NeedsAssumption);

    // And the message names the missing fact, so it is actionable.
    CHECK(ambiguous.error().message().find("assumption") != std::string::npos);
    CHECK(ambiguous.error().message().find("equal to -1") != std::string::npos);
    // Without the (mtext) marker Maxima wraps its prompts in.
    CHECK(ambiguous.error().message().find("mtext") == std::string::npos);
}

TEST_CASE("the session stays synchronised after a suppressed question") {
    const Symbol x("x");
    const Symbol n("sync_probe_n");

    REQUIRE_FALSE(proxima::integrate(pow(x, Expr(n)), x).has_value());

    CHECK(proxima::shared_kernel().ask(proxima::Query::text("2 + 2")) == Expr(4));
    CHECK(*proxima::diff(pow(x, 2), x) == 2 * Expr(x));
}

TEST_CASE("supplying the assumption lets the computation through") {
    // The point of the whole mechanism: the failure says what is missing, and
    // asking again under it is all that is needed.
    const Symbol x("x");
    const Symbol n("supply_probe_n");

    REQUIRE_FALSE(proxima::integrate(pow(x, Expr(n)), x).has_value());

    const auto integral
        = proxima::integrate(pow(x, Expr(n)), x, proxima::assuming(gt(n, 0)));
    REQUIRE(integral.has_value());
    // x^(n+1)/(n+1)
    CHECK(*proxima::simplify(*proxima::diff(*integral, x)) == pow(x, Expr(n)));
}

TEST_CASE("a fact that two things differ can be assumed") {
    // ne() used to reach assume as `n # -1`, which Maxima refuses; it wants
    // notequal(n, -1). Found writing the how-to guides.
    const Symbol x("x");
    const Symbol n("notequal_probe_n");

    const auto integral
        = proxima::integrate(pow(x, Expr(n)), x, proxima::assuming(ne(n, -1)));
    REQUIRE(integral.has_value());
    CHECK(*proxima::simplify(*proxima::diff(*integral, x)) == pow(x, Expr(n)));

    CHECK(proxima::is(ne(n, -1), proxima::assuming(ne(n, -1))) == Truth::True);
}

// --- surviving a kernel that dies ------------------------------------------------

TEST_CASE(
    "a kernel that dies answers under its assumptions again after the restart") {
    // There is nothing to replay: the assumptions come with the next question,
    // and the kernel makes their context again.
    proxima::Kernel kernel;
    const Symbol x("restart_probe");
    const auto positive = proxima::assuming(gt(x, 0));

    REQUIRE(proxima::is(gt(x, 0), {positive, kernel}) == Truth::True);

    // Ask Maxima to leave. The call itself fails, because the reply never
    // arrives.
    CHECK_THROWS_AS(
        static_cast<void>(kernel.tell(proxima::Statement::text("quit()"))),
        proxima::KernelError);

    CHECK(kernel.ask(proxima::Query::text("2 + 2")) == Expr(4));
    CHECK(proxima::is(gt(x, 0), {positive, kernel}) == Truth::True);
    CHECK(proxima::is(gt(x, 0), kernel) == Truth::Unknown);
}

TEST_CASE("a timeout loses the call, not the session") {
    proxima::Kernel kernel;

    // Tightened after startup, not before: a one-millisecond deadline would
    // otherwise time out launching Maxima, which is not a computation. That is
    // also why recovery runs on Config::startup_timeout — the deadline that was
    // just exceeded must not govern the restart that answers it.
    kernel.set_timeout(std::chrono::milliseconds(1));
    // Deliberately something that takes hundreds of milliseconds. An ordinary
    // integral finishes inside a single poll, so it would race the deadline
    // rather than reliably exceed it.
    const auto start = std::chrono::steady_clock::now();
    CHECK_THROWS_AS(
        static_cast<void>(kernel.ask(proxima::Query::text("expand((x+y+z)^200)"))),
        proxima::TimeoutError);
    // Including the restart. Recovery used to wait two seconds for the busy
    // Maxima to quit, which it never does; this call took about 2.5 s then.
    CHECK(std::chrono::steady_clock::now() - start
          < std::chrono::milliseconds(1500));

    kernel.set_timeout(std::chrono::seconds(30));
    // The restart means the next caller is not left holding a wedged kernel.
    CHECK(kernel.ask(proxima::Query::text("2 + 2")) == Expr(4));
}

} // TEST_SUITE("maxima")
