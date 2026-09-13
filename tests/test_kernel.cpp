// Integration tests: these start a real Maxima process. Excluded from the
// default unit run via `ctest -LE maxima`.

#include <doctest/doctest.h>

#include <mx/config.hpp>
#include <mx/errors.hpp>
#include <mx/expr.hpp>
#include <mx/kernel.hpp>
#include <mx/symbol.hpp>

#include "kernel/discovery.hpp"
#include "util/utf8.hpp"
#include "wire/from_maxima.hpp"
#include "wire/sexpr.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace {

/// Rewrites every head to just its operator, dropping the simplification flags
/// that follow it. Written against SExpr rather than going through fromMaxima,
/// so that the round-trip test is not comparing the mapping with itself.
mx::detail::SExpr stripSimplificationFlags(const mx::detail::SExpr &form) {
    if (!form.isList() || form.empty()) {
        return form;
    }
    std::vector<mx::detail::SExpr> items;
    items.reserve(form.size());

    const mx::detail::SExpr &head = form.at(0);
    items.push_back(head.isList() && !head.empty() ? head.at(0) : head);
    for (std::size_t i = 1; i < form.size(); ++i) {
        items.push_back(stripSimplificationFlags(form.at(i)));
    }
    return mx::detail::SExpr::list(std::move(items));
}

} // namespace

// No Maxima needed: a Reply is plain data.
TEST_CASE("toExpr reads a reply into an expression, or into its failure") {
    const auto read = mx::toExpr(mx::Reply{true, "((MPLUS SIMP) 1 $X)", ""});
    REQUIRE(read.has_value());
    CHECK(*read == mx::Expr::symbol("x") + 1);

    const auto failed = mx::toExpr(mx::Reply{false, "", "expt: undefined: 0 to a negative exponent."});
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().message == "expt: undefined: 0 to a negative exponent.");

    // Not a Maxima term at all: the protocol failing, not the mathematics.
    CHECK_THROWS_AS(static_cast<void>(mx::toExpr(mx::Reply{true, "((MPLUS", ""})),
                    mx::ParseError);
}

TEST_SUITE("maxima") {

TEST_CASE("kernel evaluates arithmetic") {
    mx::Kernel maxima;
    const mx::Reply reply = maxima.eval("1 + 1");
    REQUIRE(reply.ok);
    CHECK(reply.value == "2");
}

TEST_CASE("results arrive as Maxima's internal s-expression") {
    mx::Kernel maxima;
    // Not display output: a tagged tree with no precedence to re-derive.
    CHECK(maxima.eval("x + 1").value == "((MPLUS SIMP) 1 $X)");
    CHECK(maxima.eval("sin(x)").value == "((%SIN SIMP) $X)");
}

TEST_CASE("exact rationals are preserved rather than rounded") {
    // The single strongest reason for using the internal form. Display output
    // and every double-based expression library would give 0.7333...
    mx::Kernel maxima;
    CHECK(maxima.eval("1/3 + 2/5").value == "((RAT SIMP) 11 15)");
}

TEST_CASE("a Maxima error is reported, not thrown") {
    mx::Kernel maxima;
    const mx::Reply reply = maxima.eval("integrate(x, 5)");
    CHECK_FALSE(reply.ok);
    CHECK(reply.reason.find("must not be a number") != std::string::npos);
}

TEST_CASE("the session keeps working after an error") {
    // errcatch exists precisely so a failure does not leave the stream in an
    // error prompt, off by one for every subsequent reply.
    mx::Kernel maxima;
    REQUIRE_FALSE(maxima.eval("integrate(x, 5)").ok);
    const mx::Reply after = maxima.eval("2 + 2");
    CHECK(after.ok);
    CHECK(after.value == "4");
}

TEST_CASE("session state persists across calls") {
    // The whole point of holding the process open: a binding made by one call
    // is still there for the next.
    mx::Kernel maxima;
    REQUIRE(maxima.eval("a: 7").ok);
    CHECK(maxima.eval("a^2").value == "49");
}

TEST_CASE("kernel performs a symbolic integration") {
    mx::Kernel maxima;
    const mx::Reply reply = maxima.eval("integrate(x^2*sin(x), x)");
    REQUIRE(reply.ok);
    CHECK(reply.value.find("%SIN") != std::string::npos);
    CHECK(reply.value.find("%COS") != std::string::npos);
}

TEST_CASE("many requests in a row stay correlated") {
    // Each reply must match the request that asked for it. An off-by-one here
    // would still look entirely plausible, which is why the ids exist.
    mx::Kernel maxima;
    for (int i = 1; i <= 20; ++i) {
        const mx::Reply reply = maxima.eval(std::to_string(i) + " * 10");
        REQUIRE(reply.ok);
        CHECK(reply.value == std::to_string(i * 10));
    }
}

TEST_CASE("an unknown function comes back as an uninterpreted application") {
    // The escape hatch that keeps the term layer's node set small: anything
    // Maxima knows and we do not still round-trips.
    mx::Kernel maxima;
    CHECK(maxima.eval("f(x)").value == "(($F SIMP) $X)");
}

TEST_CASE("the launch environment reaches the child process") {
    // Maxima exposes the value of $MAXIMA_USERDIR as maxima_userdir, which
    // makes the whole environment-block path observable end to end. It is also
    // what keeps the user's own maxima-init.mac out of the picture.
    mx::Config config;
    config.userDir = std::filesystem::temp_directory_path() / "mx_test_userdir";
    const std::string expected = "\"" + config.userDir.generic_string() + "\"";

    mx::Kernel maxima(config);
    CHECK(maxima.eval("maxima_userdir").value == expected);
}

TEST_CASE("an explicitly wrong Maxima root fails loudly") {
    mx::Config config;
    config.maximaRoot
        = std::filesystem::temp_directory_path() / "no_such_maxima_install";
    CHECK_THROWS_AS(mx::Kernel{config}, mx::KernelError);
}

TEST_CASE("live replies parse as s-expressions") {
    // The golden file checks the reader against recorded output; this checks it
    // against output produced right now, which is what catches the recording
    // going stale after a Maxima upgrade.
    mx::Kernel maxima;
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
        const mx::Reply reply = maxima.eval(expression);
        REQUIRE(reply.ok);

        mx::detail::SExpr parsed;
        REQUIRE_NOTHROW(parsed = mx::detail::parseSExpr(reply.value));
        // Re-rendering must read back identically, which catches a reader that
        // accepts input but quietly loses part of it.
        CHECK(mx::detail::parseSExpr(parsed.toString()) == parsed);
    }
}

TEST_CASE("a live rational keeps its exact numerator and denominator") {
    mx::Kernel maxima;
    const mx::detail::SExpr rational
        = mx::detail::parseSExpr(maxima.eval("1/3 + 2/5").value);

    REQUIRE(rational.isList());
    CHECK(rational.at(0).at(0).isSymbol("RAT"));
    CHECK(rational.at(1).asInt64() == 11);
    CHECK(rational.at(2).asInt64() == 15);
}

TEST_CASE("a live bignum survives as digits") {
    // 30! does not fit in int64, so the reader must keep it as text rather than
    // wrap or truncate.
    mx::Kernel maxima;
    const mx::detail::SExpr value
        = mx::detail::parseSExpr(maxima.eval("30!").value);

    REQUIRE(value.isInteger());
    CHECK(value.digits() == "265252859812191058636308480000000");
    CHECK_FALSE(value.asInt64().has_value());
}

TEST_CASE("printed expressions are valid Maxima meaning the same thing") {
    // Expr::str() claims to emit Maxima-compatible infix. That claim is only
    // worth anything if Maxima agrees, so each case below is one where dropping
    // the parentheses would still *parse* but quietly mean something else.
    mx::Kernel maxima;
    const mx::Symbol x("x");
    const mx::Symbol y("y");

    const auto agreesWith = [&maxima](const mx::Expr &expr,
                                      const std::string &reference) {
        const mx::Reply printed = maxima.eval("ratsimp(" + expr.str() + ")");
        const mx::Reply expected = maxima.eval("ratsimp(" + reference + ")");
        REQUIRE_MESSAGE(printed.ok, expr.str() << " -> " << printed.reason);
        REQUIRE(expected.ok);
        return printed.value == expected.value;
    };

    // -3^2 is -9 in Maxima; the base needs its own parentheses.
    CHECK(agreesWith(pow(mx::Expr(-3), 2), "9"));
    // 3/2^2 is 3/4; (3/2)^2 is 9/4.
    CHECK(agreesWith(pow(mx::Expr::rational(3, 2), 2), "9/4"));
    // ^ is right-associative, so x^2^3 is x^8.
    CHECK(agreesWith(pow(pow(mx::Expr(x), 2), 3), "x^6"));
    // x+1*y is x+y.
    CHECK(agreesWith((mx::Expr(x) + 1) * mx::Expr(y), "(x+1)*y"));
    // x*-2 is not valid Maxima at all, so this one tests that it parses.
    CHECK(agreesWith(mx::Expr::mul({mx::Expr(x), mx::Expr(-2)}), "-2*x"));
    // A leading negative factor needs no parentheses, and must not gain any
    // that change its meaning.
    CHECK(agreesWith(-mx::Expr(x) + 1, "1-x"));
    CHECK(agreesWith(mx::Expr(x) - 3 * mx::Expr(x), "-2*x"));
    // Exact division stays exact rather than becoming a float.
    CHECK(agreesWith(mx::Expr(1) / mx::Expr(3), "1/3"));
    // Relations and uninterpreted applications survive the trip.
    CHECK(agreesWith(mx::Expr::function("bessel_j", {mx::Expr(0), mx::Expr(x)}),
                     "bessel_j(0, x)"));
}

TEST_CASE("an oversized integer round-trips through Opaque") {
    // 30! does not fit in mx::Integer, so it is carried as Opaque text. It must
    // still print as something Maxima reads back as the same number.
    mx::Kernel maxima;
    const mx::Expr bignum
        = mx::Expr::opaque("265252859812191058636308480000000");

    const mx::Reply reply = maxima.eval("is(" + bignum.str() + " = 30!)");
    REQUIRE(reply.ok);
    CHECK(reply.value == "T");
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
    mx::Kernel maxima;

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
        if (status != "OK") {
            continue;
        }
        // A bigfloat deliberately comes back as the exact rational it equals,
        // so its form changes by design. Its value is checked separately below.
        if (expression.rfind("bfloat", 0) == 0) {
            continue;
        }

        CAPTURE(expression);

        const mx::Reply first = maxima.eval(expression);
        REQUIRE(first.ok);

        const mx::Expr roundTripped
            = mx::detail::fromMaxima(mx::detail::parseSExpr(first.value));
        const std::string printed = roundTripped.str();
        CAPTURE(printed);

        const mx::Reply second = maxima.eval(printed);
        REQUIRE_MESSAGE(second.ok, printed << " -> " << second.reason);

        // Compared with the simplification flags removed. A head carries which
        // simplifiers have already touched the term — (MEXPT SIMP RATSIMP)
        // rather than (MEXPT SIMP) — which is bookkeeping about how a value was
        // reached, not part of the value. Maxima's own integrate leaves RATSIMP
        // behind where re-reading the same expression from source does not, so
        // requiring the flags to match would fail on a correct round trip.
        CHECK(stripSimplificationFlags(mx::detail::parseSExpr(second.value))
              == stripSimplificationFlags(mx::detail::parseSExpr(first.value)));
        ++cases;
    }
    CHECK(cases >= 30);
}

TEST_CASE("a bigfloat keeps its value exactly, though not its type") {
    // Mapped to the exact rational it equals, since there is no
    // arbitrary-precision float here to hold it. Nothing is rounded, so Maxima
    // agrees the two are equal even though the forms differ.
    mx::Kernel maxima;

    const mx::Reply original = maxima.eval("bfloat(%pi)");
    REQUIRE(original.ok);
    const mx::Expr asRational
        = mx::detail::fromMaxima(mx::detail::parseSExpr(original.value));

    const mx::Reply same
        = maxima.eval("is(equal(" + asRational.str() + ", bfloat(%pi)))");
    REQUIRE(same.ok);
    CHECK(same.value == "T");
}

TEST_CASE("symbol case survives the round trip in both directions") {
    // Maxima inverts case: x becomes $X and X becomes $x. A decoder that got
    // this wrong would silently rename every variable, and would look correct
    // for all-lowercase names.
    mx::Kernel maxima;
    for (const char *name : {"x", "X", "xY", "alpha", "x_1"}) {
        CAPTURE(name);
        const mx::Reply reply = maxima.eval(name);
        REQUIRE(reply.ok);

        const mx::Expr mapped
            = mx::detail::fromMaxima(mx::detail::parseSExpr(reply.value));
        CHECK(mapped.kind() == mx::Kind::Symbol);
        CHECK(mapped.name() == std::string(name));
    }
}

TEST_CASE("two kernels are independent") {
    mx::Kernel first;
    mx::Kernel second;
    REQUIRE(first.eval("b: 1").ok);
    // `second` never saw the binding, so Maxima echoes the symbol back.
    CHECK(second.eval("b").value == "$B");
}

TEST_CASE("a Maxima reached through a non-ASCII path starts and answers") {
    // The installation this machine has, reached through a link whose name
    // holds a Latin letter and two CJK characters — more than any ANSI code
    // page does — with a user directory named the same way. That exercises
    // the whole path: discovery, the launch recipe, the transport, and then
    // SBCL and Maxima, which have to decode the command line and environment
    // they are given.
    namespace fs = std::filesystem;
    const mx::detail::MaximaInstall real
        = mx::detail::discoverMaxima(mx::Config{}, mx::detail::systemEnv());

    const std::string name
        = std::string("mx_") + "m" "\xC3\xA6" "xima_" "\xE4\xB8\xAD" "\xE6\x96\x87";
    // A directory of this run's own. It used to be one fixed name, and the
    // test removes the link it finds there before making its own — so two
    // suites running at once, say a GCC and a clang-cl build, took the link
    // away from under each other's Maxima as it started.
    const fs::path base
        = fs::temp_directory_path()
          / ("maxima_cpp_unicode_test_"
             + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path link = base / mx::detail::pathFromUtf8(name);
    const fs::path userDir = base / mx::detail::pathFromUtf8(name + "_userdir");

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
        mx::Config config;
        config.maximaRoot = link;
        config.userDir = userDir;
        mx::Kernel maxima(config);

        const mx::Reply sum = maxima.eval("1 + 1");
        REQUIRE(sum.ok);
        CHECK(sum.value == "2");

        // Maxima's own idea of its user directory came from MAXIMA_USERDIR, so
        // this is the environment value after SBCL decoded it and after the
        // reply came back over the pipe.
        const mx::Reply where = maxima.eval("maxima_userdir");
        REQUIRE(where.ok);
        CAPTURE(where.value);
        CHECK(where.value.find(name + "_userdir") != std::string::npos);
    }

    fs::remove(link, ec);
    fs::remove_all(userDir, ec);
    fs::remove(base, ec);
}

TEST_CASE("a moved-from kernel reports it rather than dereferencing nothing") {
    // Moving a Kernel moves its session, and every method used to dereference
    // the empty pointer left behind: undefined behaviour, in practice a crash,
    // which is why there was no test to fail first.
    mx::Kernel original;
    mx::Kernel moved(std::move(original));

    CHECK(moved.eval("1 + 1").value == "2");
    // NOLINTBEGIN(bugprone-use-after-move): the point of the test.
    CHECK_THROWS_AS(original.eval("1 + 1"), mx::KernelError);
    CHECK_THROWS_AS(original.evalPure("1 + 1"), mx::KernelError);
    CHECK_THROWS_AS(static_cast<void>(original.cacheStats()), mx::KernelError);
    CHECK_THROWS_AS(original.restart(), mx::KernelError);

    SUBCASE("and so does one moved from by assignment") {
        mx::Kernel target;
        target = std::move(moved);
        CHECK(target.eval("2 + 2").value == "4");
        CHECK_THROWS_AS(moved.eval("2 + 2"), mx::KernelError);
    }
    // NOLINTEND(bugprone-use-after-move)
}

TEST_CASE("a large reply arrives whole") {
    // Maxima runs with Lisp's *print-length* at 100 and *print-level* at 15,
    // and the helper printed replies under those limits: a sum of 861 terms
    // came back as its first 100 and a `...`, which the reader then took for a
    // symbol. The result was a wrong answer that looked like a right one.
    mx::Kernel kernel;

    const mx::Reply wide = kernel.evalPure("expand((x+y+z)^40)");
    REQUIRE(wide.ok);
    CHECK(wide.value.find("...") == std::string::npos);
    const mx::Expr sum = mx::detail::fromMaxima(mx::detail::parseSExpr(wide.value));
    CHECK(sum.kind() == mx::Kind::Add);
    CHECK(sum.arity() == 861);

    SUBCASE("and so does a deep one") {
        // Twenty levels, past *print-level*'s fifteen, which truncates with #.
        mx::Expr nested = mx::Expr::symbol("x");
        for (int depth = 0; depth < 20; ++depth) {
            nested = mx::Expr::function("f", {nested});
        }
        const mx::Reply deep = kernel.evalPure(nested);
        REQUIRE(deep.ok);
        CHECK(mx::detail::fromMaxima(mx::detail::parseSExpr(deep.value)) == nested);
    }
}

TEST_CASE("evalExpr answers an unwrapped function with an expression") {
    mx::Kernel kernel;

    const auto gcd = kernel.evalExpr("gcd(12, 18)");
    REQUIRE(gcd.has_value());
    CHECK(*gcd == mx::Expr(6));

    SUBCASE("sent as structure too") {
        const mx::Expr x = mx::Expr::symbol("x");
        const auto derivative
            = kernel.evalExpr(mx::Expr::function("diff", {pow(x, mx::Expr(3)), x}));
        REQUIRE(derivative.has_value());
        CHECK(*derivative == 3 * pow(x, mx::Expr(2)));
    }

    SUBCASE("a Maxima error is the Failure, with Maxima's message") {
        const auto failed = kernel.evalExpr("1/0");
        REQUIRE_FALSE(failed.has_value());
        CHECK_FALSE(failed.error().message.empty());

        // Unparseable text too: read inside the error trap, so no stall.
        CHECK_FALSE(kernel.evalExpr("(1").has_value());
    }
}

} // TEST_SUITE("maxima")
