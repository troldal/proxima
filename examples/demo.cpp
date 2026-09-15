// The point of the library, in one file: symbolic mathematics through a pure
// C++ interface, with Maxima doing the work out of sight. There is no Maxima
// syntax below, and no strings standing in for expressions.
//
// Results are shown four ways near the end — infix, TeX, MathML, and
// two-dimensional text — and the last comes from examples/text2d.hpp, a
// renderer written the way any user of the library would write one.

#include <proxima/context.hpp>
#include <proxima/errors.hpp>
#include <proxima/expr.hpp>
#include <proxima/functions.hpp>
#include <proxima/mathml.hpp>
#include <proxima/numeric.hpp>
#include <proxima/ops.hpp>
#include <proxima/symbol.hpp>
#include <proxima/tex.hpp>

#include "text2d.hpp"

#include <cstddef>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

namespace {

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
void showRendered(const char *label, const proxima::Expr &expr) {
    std::cout << '\n' << label << '\n'
              << "  str()      " << expr.str() << '\n'
              << "  toTeX()    " << proxima::toTeX(expr) << '\n'
              << "  toMathML() " << proxima::toMathML(expr) << '\n'
              << "  text2d\n" << indented(text2d::draw(expr), 4) << '\n';
}

} // namespace

int main() {
    try {
        const proxima::Symbol x("x");

        // Two ways to build an expression: with operators, or from text.
        // Expr::parse needs no running Maxima.
        const proxima::Expr f = pow(proxima::Expr(x), 2) + 3 * x + 2;
        const proxima::Expr fromText = proxima::Expr::parse("x^2 + 3*x + 2");

        std::cout << "f                = " << f.str() << '\n';
        std::cout << "  from text      = " << fromText.str() << "   "
                  << (fromText == f ? "(the same expression)"
                                    : "(a different one!)")
                  << '\n';
        std::cout << "f'               = " << proxima::diff(f, x).str() << '\n';
        std::cout << "f(5)             = "
                  << proxima::subst(f, x, proxima::Expr(5)).str() << '\n';
        std::cout << "expand((x+1)^3)  = "
                  << proxima::expand(pow(x + 1, 3)).str() << '\n';
        std::cout << "factor(x^2-1)    = "
                  << proxima::factor(pow(proxima::Expr(x), 2) - 1).str() << '\n';

        // Exact arithmetic: not 0.7333...
        std::cout << "1/3 + 2/5        = "
                  << (proxima::Expr(1) / proxima::Expr(3) + proxima::Expr(2) / proxima::Expr(5)).str()
                  << '\n';

        // An integral, and the derivative of the result to check it.
        const proxima::Expr integrand = pow(proxima::Expr(x), 2) * proxima::sin(x);
        if (const auto integral = proxima::integrate(integrand, x)) {
            std::cout << "int x^2 sin(x)   = " << integral->str() << '\n';
            std::cout << "  differentiated = "
                      << proxima::ratsimp(proxima::diff(*integral, x)).str() << '\n';
        }

        if (const auto area = proxima::integrate(x * x, x, proxima::Expr(0), proxima::Expr(1))) {
            std::cout << "int_0^1 x^2      = " << area->str() << '\n';
        }

        if (const auto l = proxima::limit(proxima::sin(x) / x, x, proxima::Expr(0))) {
            std::cout << "lim sin(x)/x     = " << l->str() << '\n';
        }

        if (const auto roots = proxima::solve(eq(pow(proxima::Expr(x), 2), proxima::Expr(1)), x)) {
            std::cout << "solve x^2 = 1    = ";
            for (const proxima::Expr &root : *roots) {
                std::cout << root.str() << ' ';
            }
            std::cout << '\n';
        }

        // A system. Values come back in the order the unknowns were asked for.
        const proxima::Symbol y("y");
        const std::vector<proxima::Expr> system{eq(x + y, proxima::Expr(3)),
                                           eq(x - y, proxima::Expr(1))};
        const std::vector<proxima::Symbol> unknowns{x, y};
        if (const auto found = proxima::solve(system, unknowns)) {
            for (const proxima::Solution &solution : *found) {
                std::cout << "x+y=3, x-y=1     = x = " << solution[0].str()
                          << ", y = " << solution[1].str() << '\n';
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
        if (const auto quadratic
                = proxima::solve(eq(a * pow(proxima::Expr(x), 2) + b * x + c, proxima::Expr(0)), x);
            quadratic && !quadratic->empty()) {
            showRendered("a root of a*x^2 + b*x + c = 0:", quadratic->back());
        }
        showRendered("d/dx sin(x)/x:", proxima::diff(proxima::sin(x) / x, x));
        std::cout << '\n';

        // The other parser hands the text to Maxima itself, which accepts
        // everything its own syntax allows — but evaluates as it reads, so the
        // two answer differently.
        std::cout << "Expr::parse(5!)  = " << proxima::Expr::parse("5!").str()
                  << "   (parsed, not evaluated)\n";
        if (const auto viaMaxima = proxima::parse("5!")) {
            std::cout << "proxima::parse(5!)    = " << viaMaxima->str()
                      << "            (Maxima evaluates as it parses)\n";
        }

        // Failure is an ordinary outcome, reported rather than thrown: Maxima
        // has no closed form for this one.
        const auto hopeless = proxima::integrate(proxima::exp(proxima::sin(x)), x);
        std::cout << "int e^sin(x)     = "
                  << (hopeless ? hopeless->str()
                               : "no result: " + hopeless.error().message)
                  << '\n';

        // Some results depend on facts Maxima has not been told. Rather than
        // asking — impossible over a pipe — it says which fact is missing.
        const proxima::Symbol n("n");
        const proxima::Expr power = pow(proxima::Expr(x), proxima::Expr(n));

        const auto unknown = proxima::integrate(power, x);
        std::cout << "int x^n          = "
                  << (unknown ? unknown->str()
                              : "no result: " + unknown.error().message)
                  << '\n';

        // Supplying it in a scope, which is discarded on the way out.
        proxima::Context assuming;
        assuming.assume(gt(proxima::Expr(n), proxima::Expr(0)));
        if (const auto known = proxima::integrate(power, x)) {
            std::cout << "  assuming n > 0 = " << known->str() << '\n';
        }

        // Once a closed form exists, turning it into numbers is ordinary
        // arithmetic. No further round trips, so this is usable in a loop.
        if (const auto antiderivative = proxima::integrate(integrand, x)) {
            const auto F = proxima::asFunction(*antiderivative, x);
            std::cout << "F(1) - F(0)      = " << F(1.0) - F(0.0) << '\n';
        }
    } catch (const std::exception &e) {
        std::cerr << "Maxima call failed: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
