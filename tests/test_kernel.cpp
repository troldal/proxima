// Integration tests: these start a real Maxima process. Excluded from the
// default unit run via `ctest -LE maxima`.

#include <doctest/doctest.h>

#include <proxima/config.hpp>
#include <proxima/errors.hpp>
#include <proxima/expr.hpp>
#include <proxima/kernel.hpp>
#include <proxima/result.hpp>

#include "kernel/reply.hpp"
#include <proxima/symbol.hpp>

#include "kernel/discovery.hpp"
#include "util/utf8.hpp"
#include "wire/from_maxima.hpp"
#include "wire/sexpr.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {

/// Rewrites every head to just its operator, dropping the simplification flags
/// that follow it. Written against SExpr rather than going through from_maxima,
/// so that the round-trip test is not comparing the mapping with itself.
proxima::detail::SExpr strip_simplification_flags(const proxima::detail::SExpr &form) {
    if (!form.is_list() || form.empty()) {
        return form;
    }
    std::vector<proxima::detail::SExpr> items;
    items.reserve(form.size());

    const proxima::detail::SExpr &head = form.at(0);
    items.push_back(head.is_list() && !head.empty() ? head.at(0) : head);
    for (std::size_t i = 1; i < form.size(); ++i) {
        items.push_back(strip_simplification_flags(form.at(i)));
    }
    return proxima::detail::SExpr::list(std::move(items));
}

} // namespace

// No Maxima needed: a reply is plain data.
TEST_CASE("to_expr reads a reply into an expression, or passes its failure on") {
    const auto read = proxima::to_expr("((MPLUS SIMP) 1 $X)");
    REQUIRE(read.has_value());
    CHECK(*read == proxima::Expr::symbol("x") + 1);

    // As every operation composes it: the session's reply, made a result,
    // then read. A Maxima error is the Failure, with Maxima's wording and a
    // cause a program can branch on.
    const auto failed
        = proxima::detail::to_result(proxima::detail::Reply{
                                         false, "", "expt: undefined: 0 to a negative exponent."})
              .and_then(proxima::to_expr);
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().message() == "expt: undefined: 0 to a negative exponent.");
    CHECK(proxima::cause_of(failed.error()) == proxima::Cause::MaximaError);

    // The Lisp helper's own message, for a question Maxima could not ask,
    // has the cause that names the remedy.
    const auto question = proxima::detail::to_result(proxima::detail::Reply{
        false, "", "this computation needs an assumption that was not supplied. Maxima asked: Is n equal to -1?"});
    CHECK(proxima::cause_of(question.error()) == proxima::Cause::NeedsAssumption);

    // Not a Maxima term at all: the protocol failing, not the mathematics.
    CHECK_THROWS_AS(static_cast<void>(proxima::to_expr("((MPLUS")), proxima::ParseError);
}

TEST_SUITE("maxima") {

TEST_CASE("kernel evaluates arithmetic") {
    proxima::Kernel maxima;
    const auto reply = maxima.eval("1 + 1");
    REQUIRE(reply.has_value());
    CHECK(*reply == "2");
}

TEST_CASE("results arrive as Maxima's internal s-expression") {
    proxima::Kernel maxima;
    // Not display output: a tagged tree with no precedence to re-derive.
    CHECK(maxima.eval("x + 1").value() == "((MPLUS SIMP) 1 $X)");
    CHECK(maxima.eval("sin(x)").value() == "((%SIN SIMP) $X)");
}

TEST_CASE("exact rationals are preserved rather than rounded") {
    // The single strongest reason for using the internal form. Display output
    // and every double-based expression library would give 0.7333...
    proxima::Kernel maxima;
    CHECK(maxima.eval("1/3 + 2/5").value() == "((RAT SIMP) 11 15)");
}

TEST_CASE("a Maxima error is reported, not thrown") {
    proxima::Kernel maxima;
    const auto reply = maxima.eval("integrate(x, 5)");
    CHECK_FALSE(reply.has_value());
    CHECK(reply.error().message().find("must not be a number") != std::string::npos);
}

TEST_CASE("the session keeps working after an error") {
    // errcatch exists precisely so a failure does not leave the stream in an
    // error prompt, off by one for every subsequent reply.
    proxima::Kernel maxima;
    REQUIRE_FALSE(maxima.eval("integrate(x, 5)").has_value());
    const auto after = maxima.eval("2 + 2");
    CHECK(after.has_value());
    CHECK(*after == "4");
}

TEST_CASE("session state persists across calls") {
    // The whole point of holding the process open: a binding made by one call
    // is still there for the next.
    proxima::Kernel maxima;
    REQUIRE(maxima.eval("a: 7").has_value());
    CHECK(maxima.eval("a^2").value() == "49");
}

TEST_CASE("kernel performs a symbolic integration") {
    proxima::Kernel maxima;
    const auto reply = maxima.eval("integrate(x^2*sin(x), x)");
    REQUIRE(reply.has_value());
    CHECK(reply->find("%SIN") != std::string::npos);
    CHECK(reply->find("%COS") != std::string::npos);
}

TEST_CASE("many requests in a row stay correlated") {
    // Each reply must match the request that asked for it. An off-by-one here
    // would still look entirely plausible, which is why the ids exist.
    proxima::Kernel maxima;
    for (int i = 1; i <= 20; ++i) {
        const auto reply = maxima.eval(std::to_string(i) + " * 10");
        REQUIRE(reply.has_value());
        CHECK(*reply == std::to_string(i * 10));
    }
}

TEST_CASE("an unknown function comes back as an uninterpreted application") {
    // The escape hatch that keeps the term layer's node set small: anything
    // Maxima knows and we do not still round-trips.
    proxima::Kernel maxima;
    CHECK(maxima.eval("f(x)").value() == "(($F SIMP) $X)");
}

TEST_CASE("the launch environment reaches the child process") {
    // Maxima exposes the value of $MAXIMA_USERDIR as maxima_userdir, which
    // makes the whole environment-block path observable end to end. It is also
    // what keeps the user's own maxima-init.mac out of the picture.
    proxima::Config config;
    config.user_dir = std::filesystem::temp_directory_path() / "proxima_test_userdir";
    const std::string expected = "\"" + config.user_dir.generic_string() + "\"";

    proxima::Kernel maxima(config);
    CHECK(maxima.eval("maxima_userdir").value() == expected);
}

TEST_CASE("an explicitly wrong Maxima root fails loudly") {
    proxima::Config config;
    config.maxima_root
        = std::filesystem::temp_directory_path() / "no_such_maxima_install";
    CHECK_THROWS_AS(proxima::Kernel{config}, proxima::KernelError);
}

TEST_CASE("live replies parse as s-expressions") {
    // The golden file checks the reader against recorded output; this checks it
    // against output produced right now, which is what catches the recording
    // going stale after a Maxima upgrade.
    proxima::Kernel maxima;
    for (const char *expression : {
             "1/3 + 2/5",
             "integrate(x^2*sin(x), x)",
             "solve(x^2 - 1 = 0, x)",
             "expand((x + 1)^3)",
             "matrix([1,2],[3,4])",
             "30!",
             "bfloat(%pi)",
             "f(x, y)",
             "[a, b, c]",
         }) {
        CAPTURE(expression);
        const auto reply = maxima.eval(expression);
        REQUIRE(reply.has_value());

        proxima::detail::SExpr parsed;
        REQUIRE_NOTHROW(parsed = proxima::detail::parse_sexpr(*reply));
        // Re-rendering must read back identically, which catches a reader that
        // accepts input but quietly loses part of it.
        CHECK(proxima::detail::parse_sexpr(parsed.to_string()) == parsed);
    }
}

TEST_CASE("a live rational keeps its exact numerator and denominator") {
    proxima::Kernel maxima;
    const proxima::detail::SExpr rational
        = proxima::detail::parse_sexpr(maxima.eval("1/3 + 2/5").value());

    REQUIRE(rational.is_list());
    CHECK(rational.at(0).at(0).is_symbol("RAT"));
    CHECK(rational.at(1).as_int64() == 11);
    CHECK(rational.at(2).as_int64() == 15);
}

TEST_CASE("a live bignum survives as digits") {
    // 30! does not fit in int64, so the reader must keep it as text rather than
    // wrap or truncate.
    proxima::Kernel maxima;
    const proxima::detail::SExpr value
        = proxima::detail::parse_sexpr(maxima.eval("30!").value());

    REQUIRE(value.is_integer());
    CHECK(value.digits() == "265252859812191058636308480000000");
    CHECK_FALSE(value.as_int64().has_value());
}

TEST_CASE("printed expressions are valid Maxima meaning the same thing") {
    // Expr::str() claims to emit Maxima-compatible infix. That claim is only
    // worth anything if Maxima agrees, so each case below is one where dropping
    // the parentheses would still *parse* but quietly mean something else.
    proxima::Kernel maxima;
    const proxima::Symbol x("x");
    const proxima::Symbol y("y");

    const auto agrees_with = [&maxima](const proxima::Expr &expr,
                                      const std::string &reference) {
        const auto printed = maxima.eval("ratsimp(" + expr.str() + ")");
        const auto expected = maxima.eval("ratsimp(" + reference + ")");
        REQUIRE_MESSAGE(printed.has_value(), expr.str() << " -> " << printed.error().message());
        REQUIRE(expected.has_value());
        return *printed == *expected;
    };

    // -3^2 is -9 in Maxima; the base needs its own parentheses.
    CHECK(agrees_with(pow(proxima::Expr(-3), 2), "9"));
    // 3/2^2 is 3/4; (3/2)^2 is 9/4.
    CHECK(agrees_with(pow(proxima::Expr::rational(3, 2), 2), "9/4"));
    // ^ is right-associative, so x^2^3 is x^8.
    CHECK(agrees_with(pow(pow(proxima::Expr(x), 2), 3), "x^6"));
    // x+1*y is x+y.
    CHECK(agrees_with((proxima::Expr(x) + 1) * proxima::Expr(y), "(x+1)*y"));
    // x*-2 is not valid Maxima at all, so this one tests that it parses.
    CHECK(agrees_with(proxima::Expr::mul({proxima::Expr(x), proxima::Expr(-2)}), "-2*x"));
    // A leading negative factor needs no parentheses, and must not gain any
    // that change its meaning.
    CHECK(agrees_with(-proxima::Expr(x) + 1, "1-x"));
    CHECK(agrees_with(proxima::Expr(x) - 3 * proxima::Expr(x), "-2*x"));
    // Exact division stays exact rather than becoming a float.
    CHECK(agrees_with(proxima::Expr(1) / proxima::Expr(3), "1/3"));
    // Relations and uninterpreted applications survive the trip.
    CHECK(agrees_with(proxima::Expr::function("bessel_j", {proxima::Expr(0), proxima::Expr(x)}),
                     "bessel_j(0, x)"));
}

TEST_CASE("an oversized integer round-trips through Opaque") {
    // 30! does not fit in proxima::Integer, so it is carried as Opaque text. It must
    // still print as something Maxima reads back as the same number.
    proxima::Kernel maxima;
    const proxima::Expr bignum
        = proxima::Expr::opaque("265252859812191058636308480000000");

    const auto reply = maxima.eval("is(" + bignum.str() + " = 30!)");
    REQUIRE(reply.has_value());
    CHECK(*reply == "T");
}

TEST_CASE("every recorded expression survives a full round trip") {
    // The strongest check in the suite, and the one that actually validates the
    // mapping: for each recorded expression, evaluate it, read the internal
    // form, map it to an Expr, print that Expr back as Maxima source, evaluate
    // *that*, and require the two internal forms to be identical.
    //
    // Anything the mapping gets wrong — a mis-decoded name, a head with no
    // valid textual spelling, a lost parenthesis — shows up here as a
    // difference, or as an outright Maxima error.
    proxima::Kernel maxima;

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
        if (status != "OK") {
            continue;
        }
        // A bigfloat deliberately comes back as the exact rational it equals,
        // so its form changes by design. Its value is checked separately below.
        if (expression.rfind("bfloat", 0) == 0) {
            continue;
        }

        CAPTURE(expression);

        const auto first = maxima.eval(expression);
        REQUIRE(first.has_value());

        const proxima::Expr round_tripped
            = proxima::detail::from_maxima(proxima::detail::parse_sexpr(*first));
        const std::string printed = round_tripped.str();
        CAPTURE(printed);

        const auto second = maxima.eval(printed);
        REQUIRE_MESSAGE(second.has_value(), printed << " -> " << second.error().message());

        // Compared with the simplification flags removed. A head carries which
        // simplifiers have already touched the term — (MEXPT SIMP RATSIMP)
        // rather than (MEXPT SIMP) — which is bookkeeping about how a value was
        // reached, not part of the value. Maxima's own integrate leaves RATSIMP
        // behind where re-reading the same expression from source does not, so
        // requiring the flags to match would fail on a correct round trip.
        CHECK(strip_simplification_flags(proxima::detail::parse_sexpr(*second))
              == strip_simplification_flags(proxima::detail::parse_sexpr(*first)));
        ++cases;
    }
    CHECK(cases >= 30);
}

TEST_CASE("a bigfloat keeps its value exactly, though not its type") {
    // Mapped to the exact rational it equals, since there is no
    // arbitrary-precision float here to hold it. Nothing is rounded, so Maxima
    // agrees the two are equal even though the forms differ.
    proxima::Kernel maxima;

    const auto original = maxima.eval("bfloat(%pi)");
    REQUIRE(original.has_value());
    const proxima::Expr as_rational
        = proxima::detail::from_maxima(proxima::detail::parse_sexpr(*original));

    const auto same
        = maxima.eval("is(equal(" + as_rational.str() + ", bfloat(%pi)))");
    REQUIRE(same.has_value());
    CHECK(*same == "T");
}

TEST_CASE("symbol case survives the round trip in both directions") {
    // Maxima inverts case: x becomes $X and X becomes $x. A decoder that got
    // this wrong would silently rename every variable, and would look correct
    // for all-lowercase names.
    proxima::Kernel maxima;
    for (const char *name : {"x", "X", "xY", "alpha", "x_1"}) {
        CAPTURE(name);
        const auto reply = maxima.eval(name);
        REQUIRE(reply.has_value());

        const proxima::Expr mapped
            = proxima::detail::from_maxima(proxima::detail::parse_sexpr(*reply));
        CHECK(mapped.kind() == proxima::Kind::Symbol);
        CHECK(mapped.name() == std::string(name));
    }
}

TEST_CASE("two kernels are independent") {
    proxima::Kernel first;
    proxima::Kernel second;
    REQUIRE(first.eval("b: 1").has_value());
    // `second` never saw the binding, so Maxima echoes the symbol back.
    CHECK(second.eval("b").value() == "$B");
}

TEST_CASE("a Maxima reached through a non-ASCII path starts and answers") {
    // The installation this machine has, reached through a link whose name
    // holds a Latin letter and two CJK characters — more than any ANSI code
    // page does — with a user directory named the same way. That exercises
    // the whole path: discovery, the launch recipe, the transport, and then
    // SBCL and Maxima, which have to decode the command line and environment
    // they are given.
    namespace fs = std::filesystem;
    const proxima::detail::MaximaInstall real
        = proxima::detail::discover_maxima(proxima::Config{}, proxima::detail::system_env());

    const std::string name
        = std::string("mx_") + "m" "\xC3\xA6" "xima_" "\xE4\xB8\xAD" "\xE6\x96\x87";
    // A directory of this run's own. It used to be one fixed name, and the
    // test removes the link it finds there before making its own — so two
    // suites running at once, say a GCC and a clang-cl build, took the link
    // away from under each other's Maxima as it started.
    const fs::path base
        = fs::temp_directory_path()
          / ("proxima_unicode_test_"
             + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path link = base / proxima::detail::path_from_utf8(name);
    const fs::path user_dir = base / proxima::detail::path_from_utf8(name + "_userdir");

    // Never remove_all on `link`: it could follow the link into the real
    // installation. fs::remove takes away the link and nothing behind it.
    std::error_code ec;
    fs::create_directories(base, ec);
    fs::remove(link, ec);

    ec.clear();
    fs::create_directory_symlink(real.root, link, ec);
#ifdef _WIN32
    if (ec) {
        // A symbolic link needs Developer Mode or elevation on Windows; a
        // junction needs neither. _wsystem, so the name reaches cmd intact.
        ec.clear();
        const std::wstring command = L"mklink /J \"" + link.native() + L"\" \""
                                     + real.root.native() + L"\" >nul";
        if (_wsystem(command.c_str()) != 0) {
            ec = std::make_error_code(std::errc::operation_not_permitted);
        }
    }
#endif
    if (ec || !fs::is_directory(link)) {
        MESSAGE("Could not link to the Maxima installation; skipped.");
        return;
    }

    {
        proxima::Config config;
        config.maxima_root = link;
        config.user_dir = user_dir;
        proxima::Kernel maxima(config);

        const auto sum = maxima.eval("1 + 1");
        REQUIRE(sum.has_value());
        CHECK(*sum == "2");

        // Maxima's own idea of its user directory came from MAXIMA_USERDIR, so
        // this is the environment value after SBCL decoded it and after the
        // reply came back over the pipe.
        const auto where = maxima.eval("maxima_userdir");
        REQUIRE(where.has_value());
        CAPTURE(*where);
        CHECK(where->find(name + "_userdir") != std::string::npos);
    }

    fs::remove(link, ec);
    fs::remove_all(user_dir, ec);
    fs::remove(base, ec);
}

TEST_CASE("a moved-from kernel reports it rather than dereferencing nothing") {
    // Moving a Kernel moves its session, and every method used to dereference
    // the empty pointer left behind: undefined behaviour, in practice a crash,
    // which is why there was no test to fail first.
    proxima::Kernel original;
    proxima::Kernel moved(std::move(original));

    CHECK(moved.eval("1 + 1").value() == "2");
    // NOLINTBEGIN(bugprone-use-after-move): the point of the test.
    CHECK_THROWS_AS(static_cast<void>(original.eval("1 + 1")), proxima::KernelError);
    CHECK_THROWS_AS(static_cast<void>(original.eval_pure("1 + 1")), proxima::KernelError);
    CHECK_THROWS_AS(static_cast<void>(original.cache_stats()), proxima::KernelError);
    CHECK_THROWS_AS(original.restart(), proxima::KernelError);

    SUBCASE("and so does one moved from by assignment") {
        proxima::Kernel target;
        target = std::move(moved);
        CHECK(target.eval("2 + 2").value() == "4");
        CHECK_THROWS_AS(static_cast<void>(moved.eval("2 + 2")), proxima::KernelError);
    }
    // NOLINTEND(bugprone-use-after-move)
}

TEST_CASE("a large reply arrives whole") {
    // Maxima runs with Lisp's *print-length* at 100 and *print-level* at 15,
    // and the helper printed replies under those limits: a sum of 861 terms
    // came back as its first 100 and a `...`, which the reader then took for a
    // symbol. The result was a wrong answer that looked like a right one.
    proxima::Kernel kernel;

    const auto wide = kernel.eval_pure("expand((x+y+z)^40)");
    REQUIRE(wide.has_value());
    CHECK(wide->find("...") == std::string::npos);
    const proxima::Expr sum = proxima::detail::from_maxima(proxima::detail::parse_sexpr(*wide));
    CHECK(sum.kind() == proxima::Kind::Add);
    CHECK(sum.arity() == 861);

    SUBCASE("and so does a deep one") {
        // Twenty levels, past *print-level*'s fifteen, which truncates with #.
        proxima::Expr nested = proxima::Expr::symbol("x");
        for (int depth = 0; depth < 20; ++depth) {
            nested = proxima::Expr::function("f", {nested});
        }
        const auto deep = kernel.eval_pure(nested);
        REQUIRE(deep.has_value());
        CHECK(proxima::detail::from_maxima(proxima::detail::parse_sexpr(*deep)) == nested);
    }
}

TEST_CASE("eval_expr answers an unwrapped function with an expression") {
    proxima::Kernel kernel;

    const auto gcd = kernel.eval_expr("gcd(12, 18)");
    REQUIRE(gcd.has_value());
    CHECK(*gcd == proxima::Expr(6));

    SUBCASE("sent as structure too") {
        const proxima::Expr x = proxima::Expr::symbol("x");
        const auto derivative
            = kernel.eval_expr(proxima::Expr::function("diff", {pow(x, proxima::Expr(3)), x}));
        REQUIRE(derivative.has_value());
        CHECK(*derivative == 3 * pow(x, proxima::Expr(2)));
    }

    SUBCASE("a Maxima error is the Failure, with Maxima's message") {
        const auto failed = kernel.eval_expr("1/0");
        REQUIRE_FALSE(failed.has_value());
        CHECK_FALSE(failed.error().message().empty());

        // Unparseable text too: read inside the error trap, so no stall.
        CHECK_FALSE(kernel.eval_expr("(1").has_value());
    }
}

TEST_CASE("one Kernel shared between threads gives every caller its own answer") {
    // The README promises a Kernel is safe to share: calls take turns rather
    // than interleave on the pipe. The session's locking is tested over
    // FakeTransport; this is the promise itself, against Maxima. Every question
    // is distinct, so an answer handed to the wrong caller cannot pass.
    proxima::Kernel kernel;
    constexpr int kThreads = 4;
    constexpr int kCalls = 25;

    std::atomic<int> wrong{0};
    std::mutex first_failure_mutex;
    std::string first_failure;
    const auto fail = [&](const std::string &what) {
        ++wrong;
        const std::lock_guard<std::mutex> lock(first_failure_mutex);
        if (first_failure.empty()) {
            first_failure = what;
        }
    };

    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t] {
            try {
                for (int i = 0; i < kCalls; ++i) {
                    const int n = t * 1000 + i;
                    // A cached question and a raw eval, which clears the cache:
                    // both paths through the session's state, interleaved.
                    const auto pure = kernel.eval_pure(std::to_string(n) + " + 1");
                    if (!pure.has_value() || *pure != std::to_string(n + 1)) {
                        fail("eval_pure " + std::to_string(n) + " + 1 gave " + *pure);
                    }
                    const auto raw = kernel.eval(std::to_string(n) + " * 2");
                    if (!raw.has_value() || *raw != std::to_string(2 * n)) {
                        fail("eval " + std::to_string(n) + " * 2 gave " + *raw);
                    }
                }
            } catch (const std::exception &error) {
                fail(std::string("threw: ") + error.what());
            }
        });
    }
    for (std::thread &worker : workers) {
        worker.join();
    }

    INFO("first failure: ", first_failure);
    CHECK(wrong.load() == 0);
    // And the kernel is still one working session afterwards.
    CHECK(kernel.eval("1 + 1").value() == "2");
}

} // TEST_SUITE("maxima")
