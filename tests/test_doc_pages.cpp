// The examples on the documentation site's own pages, docs/sphinx — the
// introduction, getting started, the user guide's traversal page and the
// how-to guides — compiled and checked, as tests/test_docs.cpp does for the
// README and the pages carried over from the old guide. Each test mirrors one
// page's code, with the values its comments promise.

#include <doctest/doctest.h>

#include <proxima/assumptions.hpp>
#include <proxima/errors.hpp>
#include <proxima/functions.hpp>
#include <proxima/kernel.hpp>
#include <proxima/mathml.hpp>
#include <proxima/numeric.hpp>
#include <proxima/ops.hpp>
#include <proxima/render.hpp>
#include <proxima/tex.hpp>
#include <proxima/traverse.hpp>

#include <fxt/monads/AndThen.hpp>
#include <fxt/monads/Transform.hpp>
#include <fxt/utils/Lift.hpp>

#include <algorithm>
#include <array>
#include <complex>
#include <filesystem>
#include <format>
#include <functional>
#include <numbers>
#include <numeric>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace px = proxima;

namespace {

/// The renderer of how-to/write-a-renderer.md, as the page has it.
struct Prefix {
    std::string integer(const proxima::Integer &n) { return n.to_string(); }
    std::string real(double v) { return std::to_string(v); }
    std::string symbol(std::string_view name) { return std::string(name); }
    std::string verbatim(std::string_view source) { return std::string(source); }

    std::string sum(std::span<const proxima::Term<std::string>> terms) {
        std::string out = "(+";
        for (const auto &term : terms) {
            out += term.negated ? " (- " + term.value + ")" : " " + term.value;
        }
        return out + ")";
    }
    std::string product(std::span<const std::string> factors) {
        return "(*" + joined(factors) + ")";
    }
    std::string fraction(const std::string &n, const std::string &d) {
        return "(/ " + n + " " + d + ")";
    }
    std::string power(const std::string &b, const std::string &e) {
        return "(expt " + b + " " + e + ")";
    }
    std::string call(std::string_view head, std::span<const std::string> args) {
        return "(" + std::string(head) + joined(args) + ")";
    }
    std::string relation(proxima::RelOp op, const std::string &l,
                         const std::string &r) {
        return std::string(op == proxima::RelOp::Equal ? "(= " : "(rel ") + l + " "
               + r + ")";
    }
    std::string group(const std::string &inner) { return inner; }

private:
    static std::string joined(std::span<const std::string> parts) {
        std::string out;
        for (const auto &part : parts) {
            out += " " + part;
        }
        return out;
    }
};

} // namespace

// --- No Maxima --------------------------------------------------------------

TEST_CASE("getting started: an expression, printed") {
    const px::Symbol x("x");
    const px::Expr f = pow(x, 3) - 2 * x + 1;
    CHECK(std::format("f = {}", f) == "f = 1 + x^3 - 2*x");
    CHECK(std::format("f(2) = {}", *px::eval_numeric(f, {{x, 2.0}})) == "f(2) = 5");

    const px::Compiled fast(f, x);
    double total = 0;
    for (int i = 0; i <= 100; ++i) {
        total += fast(i / 100.0);
    }
    CHECK(total == doctest::Approx(101 - 2 * 50.5 + 25.5025).epsilon(1e-3));
}

TEST_CASE("guide/traversal: nodes, fold, rewrite and replace") {
    const px::Symbol x("x");
    const px::Expr e = pow(x, 2) + 3 * x + 2;

    const auto count = std::ranges::distance(px::nodes(e));
    CHECK(std::ranges::any_of(
        px::nodes(e), [](const px::Expr &n) { return n.is(px::Kind::Pow); }));

    const auto size = px::fold<std::size_t>(
        e, [](const px::Expr &, std::span<const std::size_t> sizes) {
            return std::accumulate(sizes.begin(), sizes.end(), std::size_t{1});
        });
    CHECK(size == static_cast<std::size_t>(count));

    const auto depth = px::fold<std::size_t>(
        e, [](const px::Expr &, std::span<const std::size_t> depths) {
            return 1 + (depths.empty() ? 0 : std::ranges::max(depths));
        });
    CHECK(depth == 3);

    const px::Expr swapped = px::rewrite(
        px::sin(x) + x, [](const px::Expr &n) -> std::optional<px::Expr> {
            if (n.is(px::Kind::Function) && n.name() == "sin") {
                return px::Expr::function("cos", {n.arg(0)});
            }
            return std::nullopt;
        });
    CHECK(swapped == px::cos(x) + x);

    CHECK(px::replace(3 * x + 2, x, px::Expr(2)) == px::Expr(8));
    CHECK(px::replace(px::sin(x), x, px::Expr(0)).str() == "sin(0)");
}

TEST_CASE("how-to/evaluate-many-points: several variables and constants") {
    const px::Symbol x("x"), y("y"), k("k");
    const std::array variables{x, y};
    const px::Compiled g(pow(x, 2) + x * y, variables);
    CHECK(g(std::array{2.0, 3.0}) == 10.0);

    const px::Compiled h(k * pow(x, 2), x, {{k, 0.5}});
    CHECK(h(4.0) == 8.0);

    const auto bad = px::compile(px::Expr::function("mystery", {x}), x);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().message().find("mystery") != std::string::npos);
}

TEST_CASE("how-to/typeset-results: TeX and MathML") {
    const px::Symbol x("x");
    const px::Expr e = (x + 1) / (x - 1);
    CHECK(px::to_tex(e) == "\\frac{1 + x}{x - 1}");
    CHECK(px::to_mathml(e).starts_with(
        R"(<math xmlns="http://www.w3.org/1998/Math/MathML"><mfrac>)"));
    CHECK(std::format("\\begin{{equation}}\n{:tex}\n\\end{{equation}}", e)
          == "\\begin{equation}\n\\frac{1 + x}{x - 1}\n\\end{equation}");
}

TEST_CASE("how-to/write-a-renderer: prefix notation") {
    const px::Symbol x("x");
    CHECK(px::render(x + 1, Prefix{}) == "(+ 1 x)");
    CHECK(px::render(pow(x, 2) / 3, Prefix{}) == "(/ (expt x 2) 3)");

    Prefix mine;
    CHECK(px::render(x + 1, std::ref(mine)) == "(+ 1 x)");
}

// --- Maxima -----------------------------------------------------------------

TEST_SUITE("maxima") {

TEST_CASE("introduction: a first look") {
    const px::Symbol x("x");
    const auto integral = px::integrate(pow(x, 2) * px::sin(x), x);
    REQUIRE(integral.has_value());
    CHECK(std::format("{}", *integral) == "cos(x)*(2 - x^2) + 2*x*sin(x)");
    CHECK(std::format("{}", *px::eval_numeric(*integral, {{x, 1.0}}))
          == "2.2232442754839328");
    CHECK(std::format("{:tex}", *integral)
              .starts_with("\\cos\\left(x\\right) \\left(2 - x^{2}\\right) + "));
}

TEST_CASE("getting started: asking Maxima") {
    const px::Symbol x("x");
    const px::Expr f = pow(x, 3) - 2 * x + 1;

    CHECK(std::format("f' = {}", *px::diff(f, x)) == "f' = 3*x^2 - 2");
    const auto F = px::integrate(f, x);
    REQUIRE(F.has_value());
    CHECK(std::format("F = {}", *F) == "F = x - x^2 + x^4/4");

    const auto hard = px::integrate(px::exp(px::sin(x)), x);
    REQUIRE_FALSE(hard.has_value());
    CHECK(hard.error().message()
          == "no closed form for the integral of %e^sin(x) with respect to x");
    CHECK(px::cause_of(hard.error()) == px::Cause::NoClosedForm);

    const auto roots = px::solve(eq(f, 0), x);
    REQUIRE(roots.has_value());
    std::vector<std::string> printed;
    for (const px::Expr &root : *roots) {
        printed.push_back(root.str());
    }
    std::ranges::sort(printed);
    CHECK(printed
          == std::vector<std::string>{"(5^(1/2) - 1)/2", "-(1 + 5^(1/2))/2", "1"});

    const px::Symbol n("n");
    CHECK_FALSE(px::integrate(pow(x, n), x).has_value());
    CHECK(px::integrate(pow(x, n), x, px::assuming(gt(n, 0)))->str()
          == "x^(1 + n)/(1 + n)");
}

TEST_CASE("how-to/solve-and-evaluate") {
    const px::Symbol x("x"), y("y");

    const auto roots = px::solve(eq(pow(x, 2), 2), x);
    REQUIRE(roots.has_value());
    REQUIRE(roots->size() == 2);
    CHECK((*roots)[0].str() == "-2^(1/2)");
    CHECK((*roots)[1].str() == "2^(1/2)");
    std::vector<double> numbers;
    for (const px::Expr &root : *roots) {
        numbers.push_back(*px::eval_numeric(root));
    }
    CHECK(numbers[0] == doctest::Approx(-1.41421356));
    CHECK(numbers[1] == doctest::Approx(1.41421356));

    const std::vector<px::Expr> system{eq(x + y, 3), eq(x - y, 1)};
    const std::vector<px::Symbol> unknowns{x, y};
    const auto found = px::solve(system, unknowns);
    REQUIRE(found.has_value());
    REQUIRE(found->size() == 1);
    CHECK((*found)[0][0] == px::Expr(2));
    CHECK((*found)[0][1] == px::Expr(1));

    const px::Expr quintic = pow(x, 5) - x - 1;
    const auto root = px::find_root(quintic, x, 1.0, 2.0);
    REQUIRE(root.has_value());
    CHECK(*root == doctest::Approx(1.1673).epsilon(1e-4));
}

TEST_CASE("how-to/solve-and-evaluate: complex roots") {
    const px::Symbol x("x");

    const auto roots = px::solve(eq(pow(x, 2), -1), x);
    REQUIRE(roots.has_value());
    REQUIRE(roots->size() == 2);
    CHECK((*roots)[0] == -px::i());
    CHECK(*px::eval_complex((*roots)[0]) == std::complex<double>(0, -1));
    CHECK(*px::eval_complex((*roots)[1]) == std::complex<double>(0, 1));
    CHECK_FALSE(px::eval_numeric((*roots)[0]).has_value());

    // Three real roots, written with %i.
    const px::Expr cubic = pow(x, 3) - 3 * x + 1;
    const auto real_roots = px::solve(eq(cubic, 0), x);
    REQUIRE(real_roots.has_value());
    REQUIRE(real_roots->size() == 3);
    for (const px::Expr &r : *real_roots) {
        CAPTURE(r.str());
        CHECK(px::contains(r, px::Symbol("%i")));
        const std::complex<double> z = *px::eval_complex(r);
        CHECK(std::abs(z.imag()) < 1e-12);
        CHECK(*px::eval_numeric(cubic, {{x, z.real()}})
              == doctest::Approx(0.0).epsilon(1e-9));
    }
}

TEST_CASE("guide/numeric: complex numbers and Maxima's help") {
    const px::Symbol x("x");
    CHECK(*px::eval_complex(px::log(px::Expr(-1)))
          == std::complex<double>(0, std::numbers::pi));

    const px::Expr j0 = px::Expr::function("bessel_j", {px::Expr(0), x});
    CHECK(*px::to_double(j0, {{x, 1.0}}) == doctest::Approx(0.765198).epsilon(1e-6));
    CHECK(*px::to_double(px::sin(x), {{x, 1.0}})
          == doctest::Approx(0.841471).epsilon(1e-6));
}

TEST_CASE("how-to/evaluate-many-points: from an integral") {
    const px::Symbol x("x");
    const px::Expr F = *px::integrate(pow(x, 2) * px::sin(x), x);
    const px::Compiled f(F, x);
    std::vector<double> table;
    for (int i = 0; i <= 1000; ++i) {
        table.push_back(f(i * 0.001));
    }
    CHECK(table.back() == doctest::Approx(2.22324).epsilon(1e-5));
    const auto c = px::compile(F, x);
    REQUIRE(c.has_value());
    CHECK((*c)(1.0) == table.back());
}

TEST_CASE("how-to/supply-missing-facts") {
    const px::Symbol x("x"), n("n");
    const auto first = px::integrate(pow(x, n), x);
    REQUIRE_FALSE(first.has_value());
    CHECK(first.error().message()
          == "this computation needs an assumption that was not supplied. Maxima "
             "asked: Is n equal to -1?");

    CHECK(px::integrate(pow(x, n), x, px::assuming(gt(n, 0)))->str()
          == "x^(1 + n)/(1 + n)");
    CHECK(px::integrate(pow(x, n), x, px::assuming(ne(n, -1)))->str()
          == "x^(1 + n)/(1 + n)");

    auto antiderivative = px::integrate(pow(x, n), x);
    if (!antiderivative
        && px::cause_of(antiderivative.error()) == px::Cause::NeedsAssumption) {
        antiderivative = px::integrate(pow(x, n), x, px::assuming(gt(n, 0)));
    }
    CHECK(antiderivative.has_value());

    CHECK(px::is(gt(pow(n, 2), 0), px::assuming(gt(n, 0))) == px::Truth::True);
    CHECK(px::is(gt(n, 0)) == px::Truth::Unknown);
}

TEST_CASE("how-to/handle-failures") {
    const px::Symbol x("x");
    const auto integral = px::integrate(px::exp(px::sin(x)), x);
    REQUIRE_FALSE(integral.has_value());
    CHECK(integral.value_or(px::Expr::symbol("unknown"))
          == px::Expr::symbol("unknown"));
    CHECK_THROWS_AS(static_cast<void>(px::unwrap(integral)), px::Error);

    const px::Expr f = pow(x, 3);
    const auto tex = px::diff(f, x) | fxt::and_then(FXT_LIFT(px::factor))
                     | fxt::transform(px::to_tex);
    CHECK(tex == std::string("3 x^{2}"));
}

TEST_CASE("how-to/use-threads") {
    const px::Symbol x("x");
    const std::vector<px::Expr> integrands{px::sin(x), px::cos(x), px::exp(x),
                                           1 / x};
    std::vector<px::Expr> results(integrands.size());
    {
        std::vector<std::jthread> workers;
        for (std::size_t t = 0; t < 2; ++t) {
            workers.emplace_back([&, t] {
                px::Kernel kernel;
                for (std::size_t i = t; i < integrands.size(); i += 2) {
                    results[i] = *px::integrate(integrands[i], x, kernel);
                }
            });
        }
    }
    CHECK(results[0] == -px::cos(x));
    CHECK(results[3] == px::log(x));
}

TEST_CASE("how-to/cache-between-runs") {
    const auto directory
        = std::filesystem::temp_directory_path() / "proxima-doc-pages-cache";
    std::filesystem::remove_all(directory);
    px::Config config;
    config.cache_directory = directory;
    const px::Symbol x("x");
    {
        px::Kernel kernel(config);
        CHECK(px::integrate(pow(x, 2) * px::sin(x), x, kernel).has_value());
        CHECK(kernel.persistence_active());
    }
    {
        px::Kernel again(config);
        CHECK(px::integrate(pow(x, 2) * px::sin(x), x, again).has_value());
        CHECK(again.cache_stats().persistent_hits == 1);
    }
    std::filesystem::remove_all(directory);
}

TEST_CASE("guide/parsing: proxima::parse simplifies, ask evaluates") {
    const px::Symbol x("x");
    CHECK(px::parse("5!") == px::Expr(120));
    CHECK(px::parse("diff(x^2, x)")->str() == "diff(x^2, x)");
    CHECK(px::shared_kernel().ask(px::Query::text("diff(x^2, x)"))
          == 2 * px::Expr(x));
}

TEST_CASE("how-to/call-unwrapped-maxima: what to watch for in text") {
    px::Kernel kernel;
    CHECK(kernel.ask(px::Query::text("diff(x^2, x, 1)"))
          == 2 * px::Expr(px::Symbol("x")));
    CHECK(kernel.ask(px::Query::text("block([a: 2], a + 1)")) == px::Expr(3));
    // Only the first statement counts; the roadmap has this as a hole.
    CHECK(kernel.ask(px::Query::text("b: 2$ b + 1")) == px::Expr(2));
    const auto broken = kernel.ask(px::Query::text("diff(x^2,x,"));
    REQUIRE_FALSE(broken.has_value());
    CHECK(px::cause_of(broken.error()) == px::Cause::MaximaError);
}

TEST_CASE("how-to/call-unwrapped-maxima") {
    px::Kernel kernel;
    CHECK(kernel.ask(px::Query::text("gcd(12, 18)")) == px::Expr(6));
    CHECK(kernel.ask(px::Query::form(px::Expr::function("gcd", {12, 18})))
          == px::Expr(6));

    const px::Symbol x("x");
    CHECK(kernel.ask(px::Query::form(px::Expr::function("abs", {x})),
                     px::assuming(gt(x, 0)))
          == px::Expr(x));

    REQUIRE(kernel.tell(px::Statement::text("load(\"distrib\")")).has_value());
    CHECK(kernel.ask(px::Query::text("cdf_normal(0, 0, 1)"))
          == px::Expr(1) / px::Expr(2));
}

} // TEST_SUITE("maxima")
