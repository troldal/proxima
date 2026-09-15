// Assumption scopes, and the interactive-question hazard they exist to answer.
// All of this needs a real kernel.

#include <doctest/doctest.h>

#include <proxima/context.hpp>
#include <proxima/errors.hpp>
#include <proxima/expr.hpp>
#include <proxima/functions.hpp>
#include <proxima/ops.hpp>
#include <proxima/symbol.hpp>

#include <chrono>
#include <string>

using proxima::Context;
using proxima::Expr;
using proxima::Feature;
using proxima::Symbol;

TEST_SUITE("maxima") {

TEST_CASE("an assumption changes what Maxima can conclude") {
    const Symbol x("x");

    // Without knowing the sign, the best Maxima can do is |x|.
    CHECK(proxima::simplify(proxima::sqrt(pow(Expr(x), 2))) == proxima::abs(Expr(x)));

    {
        Context ctx;
        ctx.assume(gt(Expr(x), Expr(0)));
        CHECK(proxima::simplify(proxima::sqrt(pow(Expr(x), 2))) == Expr(x));
    }

    // And the scope really is a scope.
    CHECK(proxima::simplify(proxima::sqrt(pow(Expr(x), 2))) == proxima::abs(Expr(x)));
}

TEST_CASE("a declaration is undone too, which forget would not manage") {
    const Symbol n("n");

    {
        Context ctx;
        ctx.declare(n, Feature::Integer);
        CHECK(proxima::sharedKernel().eval("featurep(n, integer)").value == "T");
    }
    CHECK(proxima::sharedKernel().eval("featurep(n, integer)").value == "NIL");
}

TEST_CASE("contexts nest, inheriting the enclosing scope's facts") {
    const Symbol a("ctx_a");
    const Symbol b("ctx_b");

    Context outer;
    outer.assume(gt(Expr(a), Expr(0)));

    {
        Context inner;
        inner.assume(gt(Expr(b), Expr(0)));
        // The inner scope can see both, which is what supcontext buys over
        // newcontext.
        CHECK(proxima::sharedKernel().eval("is(ctx_a > 0)").value == "T");
        CHECK(proxima::sharedKernel().eval("is(ctx_b > 0)").value == "T");
    }

    // Leaving the inner scope discards only its own assumption.
    CHECK(proxima::sharedKernel().eval("is(ctx_a > 0)").value == "T");
    CHECK(proxima::sharedKernel().eval("is(ctx_b > 0)").value != "T");
}

TEST_CASE("facts reports what is in force") {
    const Symbol x("facts_probe");

    Context ctx;
    CHECK(ctx.assumptions().empty());

    ctx.assume(gt(Expr(x), Expr(0)));
    CHECK(ctx.assumptions().size() == 1);

    const std::vector<Expr> facts = ctx.facts();
    CHECK(facts.size() >= 1);
    CHECK(facts.front().kind() == proxima::Kind::Relation);
}

TEST_CASE("facts lists this scope's facts and every enclosing scope's") {
    // Maxima's facts(name) lists one context's facts and a bare facts() lists
    // the current context's. facts() used to be the bare call, so it disagreed
    // with its documentation twice over: an inner scope did not report what it
    // inherited, and an outer scope reported whichever scope was current.
    const Symbol a("lineage_a");
    const Symbol b("lineage_b");
    const Symbol k("lineage_k");
    const Expr outerFact = gt(Expr(a), Expr(0));
    const Expr innerFact = gt(Expr(b), Expr(0));
    const Expr declaration = Expr::function("kind", {Expr(k), Expr::symbol("integer")});

    const auto position = [](const std::vector<Expr> &facts, const Expr &fact) {
        for (std::size_t i = 0; i < facts.size(); ++i) {
            if (facts[i] == fact) {
                return static_cast<long>(i);
            }
        }
        return -1L;
    };

    Context outer;
    outer.assume(outerFact);
    {
        Context inner;
        inner.assume(innerFact);
        inner.declare(k, Feature::Integer);

        const std::vector<Expr> innerFacts = inner.facts();
        CHECK(position(innerFacts, innerFact) >= 0);
        CHECK(position(innerFacts, declaration) >= 0);
        CHECK(position(innerFacts, outerFact) >= 0);
        // Innermost first.
        CHECK(position(innerFacts, innerFact) < position(innerFacts, outerFact));

        // The outer scope, asked while the inner one is current, describes
        // itself.
        const std::vector<Expr> outerFacts = outer.facts();
        CHECK(position(outerFacts, outerFact) >= 0);
        CHECK(position(outerFacts, innerFact) < 0);
        CHECK(position(outerFacts, declaration) < 0);
    }

    // Maxima's own type facts, from its `global` context, are not assumptions.
    const Expr builtIn
        = Expr::function("kind", {Expr::symbol("%e"), Expr::symbol("irrational")});
    CHECK(position(outer.facts(), builtIn) < 0);
}

TEST_CASE("contexts ended out of order leave the survivors intact") {
    // Nothing forces scopes to end innermost first: a Context on the heap, or
    // in another object, can outlive the one it was opened inside. The inner
    // scope was opened inheriting the outer one's facts, so it must keep them
    // for as long as it lives, and both must be gone once both have ended.
    //
    // A kernel of its own, so that what is left over afterwards can be checked
    // without other tests' scopes getting in the way.
    proxima::Kernel kernel;
    const Symbol a("order_a");
    const Symbol b("order_b");

    auto outer = std::make_unique<Context>(kernel);
    outer->assume(gt(Expr(a), Expr(0)));
    auto inner = std::make_unique<Context>(kernel);
    inner->assume(gt(Expr(b), Expr(0)));

    outer.reset(); // The outer scope ends first.
    CHECK(kernel.eval("is(order_b > 0)").value == "T");
    CHECK(kernel.eval("is(order_a > 0)").value == "T");

    inner.reset();
    CHECK(kernel.eval("is(order_a > 0)").value != "T");
    CHECK(kernel.eval("is(order_b > 0)").value != "T");
    CHECK(kernel.eval("context").value == "$INITIAL");
    CHECK(kernel.eval("contexts").value.find("PROXIMA_CTX") == std::string::npos);
}

TEST_CASE("a restart after an out-of-order end rebuilds the surviving scope") {
    // Replay has to rebuild the inner scope, and the inner scope is a
    // subcontext of the outer one — so the outer one's record must outlive the
    // outer Context object for as long as the inner scope needs it.
    proxima::Kernel kernel;
    const Symbol a("order_restart_a");
    const Symbol b("order_restart_b");

    auto outer = std::make_unique<Context>(kernel);
    outer->assume(gt(Expr(a), Expr(0)));
    auto inner = std::make_unique<Context>(kernel);
    inner->assume(gt(Expr(b), Expr(0)));
    outer.reset();

    CHECK_THROWS_AS(kernel.eval("quit()"), proxima::KernelError);
    CHECK(kernel.eval("is(order_restart_b > 0)").value == "T");
    CHECK(kernel.eval("is(order_restart_a > 0)").value == "T");

    inner.reset();
    CHECK(kernel.eval("is(order_restart_a > 0)").value != "T");
    CHECK(kernel.eval("context").value == "$INITIAL");
}

TEST_CASE("a Context that outlives its Kernel reports it instead of calling into it") {
    // The situation a Context with static storage duration is in at exit,
    // once sharedKernel() has been destroyed. It used to call into the
    // destroyed Kernel — a use after free, and so a crash rather than a
    // failing check, which is why this test could not be run first.
    const Symbol x("outlived_probe");

    auto kernel = std::make_unique<proxima::Kernel>();
    auto outer = std::make_unique<Context>(*kernel);
    outer->assume(gt(Expr(x), Expr(0)));
    auto inner = std::make_unique<Context>(*kernel);

    kernel.reset();

    CHECK_THROWS_AS(inner->assume(gt(Expr(x), Expr(1))), proxima::KernelError);
    CHECK_THROWS_AS(inner->declare(x, Feature::Integer), proxima::KernelError);
    CHECK_THROWS_AS(static_cast<void>(inner->facts()), proxima::KernelError);

    // Ending them, outer first, has nothing to tidy and touches nothing.
    CHECK_NOTHROW(outer.reset());
    CHECK_NOTHROW(inner.reset());

    // And nothing left behind trips up the next kernel's scopes.
    proxima::Kernel fresh;
    {
        Context again(fresh);
        again.assume(gt(Expr(x), Expr(0)));
        CHECK(fresh.eval("is(outlived_probe > 0)").value == "T");
    }
    CHECK(fresh.eval("is(outlived_probe > 0)").value != "T");
    CHECK(fresh.eval("context").value == "$INITIAL");
}

TEST_CASE("a contradictory assumption is refused") {
    // Maxima detects the contradiction; carrying on with an inconsistent set of
    // facts would make every later result in the scope meaningless.
    const Symbol x("contradiction_probe");

    Context ctx;
    ctx.assume(gt(Expr(x), Expr(0)));
    CHECK_THROWS_AS(ctx.assume(lt(Expr(x), Expr(0))), proxima::MaximaError);
}

TEST_CASE("a redundant assumption is accepted quietly") {
    const Symbol x("redundant_probe");

    Context ctx;
    ctx.assume(gt(Expr(x), Expr(0)));
    CHECK_NOTHROW(ctx.assume(gt(Expr(x), Expr(0))));
}

// --- the hazard contexts exist to answer -----------------------------------

TEST_CASE("a question Maxima would ask becomes an error, not a deadlock") {
    // integrate(x^n, x) cannot proceed without knowing whether n is -1, and
    // Maxima's way of finding out is to print a prompt and read a line. Over a
    // pipe that blocks until the timeout and then swallows the *next* request
    // as the answer, desynchronising every reply after it.
    //
    // Overriding Maxima's `retrieve` turns the question into an ordinary error.
    const Symbol x("x");
    const Symbol n("question_probe_n");

    const auto ambiguous = proxima::integrate(pow(Expr(x), Expr(n)), x);
    REQUIRE_FALSE(ambiguous.has_value());

    // And the message names the missing fact, so it is actionable.
    CHECK(ambiguous.error().message.find("assumption") != std::string::npos);
    CHECK(ambiguous.error().message.find("equal to -1") != std::string::npos);
    // Without the (mtext) marker Maxima wraps its prompts in.
    CHECK(ambiguous.error().message.find("mtext") == std::string::npos);
}

TEST_CASE("the session stays synchronised after a suppressed question") {
    // The real damage a blocking prompt would do is not the one failed call but
    // every call after it answering the wrong question.
    const Symbol x("x");
    const Symbol n("sync_probe_n");

    REQUIRE_FALSE(proxima::integrate(pow(Expr(x), Expr(n)), x).has_value());

    CHECK(proxima::sharedKernel().eval("2 + 2").value == "4");
    CHECK(proxima::diff(pow(Expr(x), 2), x) == 2 * Expr(x));
    CHECK(proxima::sharedKernel().eval("6*7").value == "42");
}

TEST_CASE("supplying the assumption lets the computation through") {
    // The point of the whole mechanism: the error says what is missing, and a
    // Context supplies it.
    const Symbol x("x");
    const Symbol n("supply_probe_n");

    REQUIRE_FALSE(proxima::integrate(pow(Expr(x), Expr(n)), x).has_value());

    Context ctx;
    ctx.assume(gt(Expr(n), Expr(0)));

    const auto integral = proxima::integrate(pow(Expr(x), Expr(n)), x);
    REQUIRE(integral.has_value());
    // x^(n+1)/(n+1)
    CHECK(proxima::simplify(proxima::diff(*integral, x)) == pow(Expr(x), Expr(n)));
}

// --- surviving a kernel that dies -----------------------------------------

TEST_CASE("a kernel that dies is restarted with its assumptions intact") {
    // The failure this guards against is not the crash but what comes after it.
    // A kernel that came back *working* yet missing the caller's assumptions
    // would answer every later question confidently and wrongly, with nothing
    // to announce that anything had happened.
    //
    // Uses its own kernel rather than the shared one, so killing it cannot
    // disturb the other tests.
    proxima::Kernel kernel;
    const Symbol x("restart_probe");

    Context ctx(kernel);
    ctx.assume(gt(Expr(x), Expr(0)));
    REQUIRE(kernel.eval("is(restart_probe > 0)").value == "T");

    // Ask Maxima to leave. The call itself fails, because the reply never
    // arrives.
    CHECK_THROWS_AS(kernel.eval("quit()"), proxima::KernelError);

    // But the session is usable again...
    CHECK(kernel.eval("2 + 2").value == "4");
    // ...and the assumption survived the restart.
    CHECK(kernel.eval("is(restart_probe > 0)").value == "T");
}

TEST_CASE("an assumption dropped before a death does not come back") {
    proxima::Kernel kernel;
    const Symbol x("restart_scope_probe");

    {
        Context ctx(kernel);
        ctx.assume(gt(Expr(x), Expr(0)));
        REQUIRE(kernel.eval("is(restart_scope_probe > 0)").value == "T");
    }

    CHECK_THROWS_AS(kernel.eval("quit()"), proxima::KernelError);
    CHECK(kernel.eval("2 + 2").value == "4");
    // The scope had ended, so replay must not resurrect it.
    CHECK(kernel.eval("is(restart_scope_probe > 0)").value != "T");
}

TEST_CASE("a timeout loses the call, not the session") {
    proxima::Kernel kernel;

    // Tightened after startup, not before: a one-millisecond deadline would
    // otherwise time out launching Maxima, which is not a computation. That is
    // also why recovery runs on Config::startupTimeout — the deadline that was
    // just exceeded must not govern the restart that answers it.
    kernel.setTimeout(std::chrono::milliseconds(1));
    // Deliberately something that takes hundreds of milliseconds. An ordinary
    // integral finishes inside a single poll, so it would race the deadline
    // rather than reliably exceed it.
    const auto start = std::chrono::steady_clock::now();
    CHECK_THROWS_AS(kernel.eval("expand((x+y+z)^200)"), proxima::TimeoutError);
    // Including the restart. Recovery used to wait two seconds for the busy
    // Maxima to quit, which it never does; this call took about 2.5 s then.
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(1500));

    kernel.setTimeout(std::chrono::seconds(30));
    // The restart means the next caller is not left holding a wedged kernel.
    CHECK(kernel.eval("2 + 2").value == "4");
}

} // TEST_SUITE("maxima")
