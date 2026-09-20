// The point of the library, in one file: symbolic mathematics through a pure
// C++ interface, with Maxima doing the work out of sight. There is no Maxima
// syntax below, and no strings standing in for expressions.
//
// Results are shown four ways near the end — infix, TeX, MathML, and
// two-dimensional text — and the last comes from examples/text2d.hpp, a
// renderer written the way any user of the library would write one.

#include <proxima/assumptions.hpp>
#include <proxima/config.hpp>
#include <proxima/errors.hpp>
#include <proxima/expr.hpp>
#include <proxima/functions.hpp>
#include <proxima/kernel.hpp>
#include <proxima/mathml.hpp>
#include <proxima/numeric.hpp>
#include <proxima/ops.hpp>
#include <proxima/symbol.hpp>
#include <proxima/tex.hpp>

#include "text2d.hpp"

#include <cstddef>
#include <cstdio>
#include <exception>
#include <print>
#include <string>
#include <vector>

namespace {

// Where Maxima is. Fill these in to say, rather than have Proxima look:
//
//     kSbclExe    = "C:/maxima-5.50.0/bin/sbcl.exe";
//     kMaximaCore = "C:/maxima-5.50.0/lib/maxima/5.50.0/binary-sbcl/maxima.core";
//
// Naming the SBCL runtime and the Maxima core means nothing is searched for
// and no directory layout is assumed, which is what an application shipping
// its own copy of Maxima does — its installer knows where it put them. A real
// program would read them from its own configuration rather than compile them
// in. Naming kMaximaRoot instead leaves the layout to Proxima.
//
// Empty, as they are here so that this demo runs anywhere, Proxima discovers
// the installation: $MAXIMA_ROOT, a launcher on $PATH, then the conventional
// locations.
constexpr const char *kSbclExe = "";
constexpr const char *kMaximaCore = "";
constexpr const char *kMaximaRoot = "";

proxima::Config maxima_location() {
    proxima::Config config;
    config.sbcl_exe = kSbclExe;
    config.maxima_core = kMaximaCore;
    config.maxima_root = kMaximaRoot;
    return config;
}

/// Says which of the three the run is using, before anything is asked.
void report_location(const proxima::Config &config) {
    if (!config.sbcl_exe.empty() || !config.maxima_core.empty()) {
        std::println("Maxima: named outright\n  sbcl = {}\n  core = {}",
                     config.sbcl_exe.string(), config.maxima_core.string());
    } else if (!config.maxima_root.empty()) {
        std::println("Maxima: under the root {}", config.maxima_root.string());
    } else {
        std::println("Maxima: discovered (fill in kSbclExe and kMaximaCore "
                     "above to name it)");
    }
}

/// Indents every line of a multi-line block, so a drawn expression sits
/// under its label.
std::string indented(const std::string &block, std::size_t by) {
    const std::string pad(by, ' ');
    std::string out = pad;
    for (const char ch : block) {
        out += ch;
        if (ch == '\n') {
            out += pad;
        }
    }
    return out;
}

/// One expression, in each of the four renderers.
void show_rendered(const char *label, const proxima::Expr &expr) {
    std::println("\n{}", label);
    std::println("  str()      {}", expr);
    std::println("  to_tex()    {}", proxima::to_tex(expr));
    std::println("  to_mathml() {}", proxima::to_mathml(expr));
    std::println("  text2d\n{}", indented(text2d::draw(expr), 4));
}

} // namespace

int main() {
    try {
        // Every operation below is asked of this kernel, rather than of the
        // shared one, so the location above is what it uses.
        const proxima::Config config = maxima_location();
        report_location(config);
        proxima::Kernel kernel(config);

        const proxima::Symbol x("x");

        // Two ways to build an expression: with operators, or from text.
        // Expr::parse needs no running Maxima.
        const proxima::Expr f = pow(x, 2) + 3 * x + 2;
        const proxima::Expr from_text = *proxima::Expr::parse("x^2 + 3*x + 2");

        std::println("f                = {}", f);
        std::println("  from text      = {}   {}", from_text,
                     from_text == f ? "(the same expression)"
                                    : "(a different one!)");
        std::println("f'               = {}", *proxima::diff(f, x, 1, kernel));
        std::println("f(5)             = {}",
                     *proxima::subst(f, x, proxima::Expr(5), kernel));
        std::println("expand((x+1)^3)  = {}",
                     *proxima::expand(pow(x + 1, 3), kernel));
        std::println("factor(x^2-1)    = {}",
                     *proxima::factor(pow(x, 2) - 1, kernel));

        // Exact arithmetic: not 0.7333...
        std::println("1/3 + 2/5        = {}",
                     proxima::Expr(1) / proxima::Expr(3)
                         + proxima::Expr(2) / proxima::Expr(5));

        // An integral, and the derivative of the result to check it.
        const proxima::Expr integrand = pow(x, 2) * proxima::sin(x);
        if (const auto integral = proxima::integrate(integrand, x, kernel)) {
            std::println("int x^2 sin(x)   = {}", *integral);
            std::println(
                "  differentiated = {}",
                *proxima::ratsimp(*proxima::diff(*integral, x, 1, kernel), kernel));
        }

        if (const auto area = proxima::integrate(x * x, x, proxima::Expr(0),
                                                 proxima::Expr(1), kernel)) {
            std::println("int_0^1 x^2      = {}", *area);
        }

        if (const auto l = proxima::limit(proxima::sin(x) / x, x, proxima::Expr(0),
                                          proxima::Side::Both, kernel)) {
            std::println("lim sin(x)/x     = {}", *l);
        }

        if (const auto roots
            = proxima::solve(eq(pow(x, 2), proxima::Expr(1)), x, kernel)) {
            std::print("solve x^2 = 1    = ");
            for (const proxima::Expr &root : *roots) {
                std::print("{} ", root);
            }
            std::println();
        }

        // A system. Values come back in the order the unknowns were asked for.
        const proxima::Symbol y("y");
        const std::vector<proxima::Expr> system{eq(x + y, proxima::Expr(3)),
                                                eq(x - y, proxima::Expr(1))};
        const std::vector<proxima::Symbol> unknowns{x, y};
        if (const auto found = proxima::solve(system, unknowns, kernel)) {
            for (const proxima::Solution &solution : *found) {
                std::println("x+y=3, x-y=1     = x = {}, y = {}", solution[0],
                             solution[1]);
            }
        }

        // Rendering. str() is one renderer among several. TeX and MathML ship
        // with the library; text2d::Renderer does not — it is a plain struct
        // in examples/text2d.hpp that inherits nothing and that the library
        // has never heard of. The same expression goes through all four, and
        // the decisions about where brackets go are shared by every one.
        const proxima::Symbol a("a");
        const proxima::Symbol b("b");
        const proxima::Symbol c("c");
        if (const auto quadratic = proxima::solve(
                eq(a * pow(x, 2) + b * x + c, proxima::Expr(0)), x, kernel);
            quadratic && !quadratic->empty()) {
            show_rendered("a root of a*x^2 + b*x + c = 0:", quadratic->back());
        }
        show_rendered("d/dx sin(x)/x:",
                      *proxima::diff(proxima::sin(x) / x, x, 1, kernel));
        std::println();

        // The other parser hands the text to Maxima itself, which accepts
        // everything its own syntax allows — and simplifies what it reads, so
        // the two answer differently.
        std::println("Expr::parse(5!)   = {}   (parsed, not evaluated)",
                     *proxima::Expr::parse("5!"));
        if (const auto via_maxima = proxima::parse("5!", kernel)) {
            std::println("proxima::parse(5!)    = {}            (Maxima simplifies "
                         "as it reads)",
                         *via_maxima);
        }

        // Failure is an ordinary outcome, reported rather than thrown: Maxima
        // has no closed form for this one.
        const auto hopeless
            = proxima::integrate(proxima::exp(proxima::sin(x)), x, kernel);
        std::println("int e^sin(x)     = {}",
                     hopeless ? hopeless->str()
                              : "no result: " + hopeless.error().message());

        // Some results depend on facts Maxima has not been told. Rather than
        // asking — impossible over a pipe — it says which fact is missing.
        const proxima::Symbol n("n");
        const proxima::Expr power = pow(x, n);

        const auto unknown = proxima::integrate(power, x, kernel);
        std::println("int x^n          = {}",
                     unknown ? unknown->str()
                             : "no result: " + unknown.error().message());

        // Supplying it with the question.
        if (const auto known
            = proxima::integrate(power, x, {proxima::assuming(gt(n, 0)), kernel})) {
            std::println("  assuming n > 0 = {}", *known);
        }

        // Once a closed form exists, turning it into numbers is ordinary
        // arithmetic. No further round trips, so this is usable in a loop.
        if (const auto antiderivative = proxima::integrate(integrand, x, kernel)) {
            const auto F = proxima::as_function(*antiderivative, x);
            std::println("F(1) - F(0)      = {:.6g}", F(1.0) - F(0.0));
        }
    } catch (const std::exception &e) {
        std::println(stderr, "Maxima call failed: {}", e.what());
        return 1;
    }

    return 0;
}
