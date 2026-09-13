// The point of the library, in one file: symbolic mathematics through a pure
// C++ interface, with Maxima doing the work out of sight. There is no Maxima
// syntax below, and no strings standing in for expressions.
//
// Results are shown four ways near the end — infix, TeX, MathML, and
// two-dimensional text — and the last comes from examples/text2d.hpp, a
// renderer written the way any user of the library would write one.

#include <mx/context.hpp>
#include <mx/errors.hpp>
#include <mx/expr.hpp>
#include <mx/functions.hpp>
#include <mx/mathml.hpp>
#include <mx/numeric.hpp>
#include <mx/ops.hpp>
#include <mx/symbol.hpp>
#include <mx/tex.hpp>

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
void showRendered(const char *label, const mx::Expr &expr) {
    std::cout << '\n' << label << '\n'
              << "  str()      " << expr.str() << '\n'
              << "  toTeX()    " << mx::toTeX(expr) << '\n'
              << "  toMathML() " << mx::toMathML(expr) << '\n'
              << "  text2d\n" << indented(text2d::draw(expr), 4) << '\n';
}

} // namespace

int main() {
    try {
        const mx::Symbol x("x");

        // Two ways to build an expression: with operators, or from text.
        // Expr::parse needs no running Maxima.
        const mx::Expr f = pow(mx::Expr(x), 2) + 3 * x + 2;
        const mx::Expr fromText = mx::Expr::parse("x^2 + 3*x + 2");

        std::cout << "f                = " << f.str() << '\n';
        std::cout << "  from text      = " << fromText.str() << "   "
                  << (fromText == f ? "(the same expression)"
                                    : "(a different one!)")
                  << '\n';
        std::cout << "f'               = " << mx::diff(f, x).str() << '\n';
        std::cout << "f(5)             = "
                  << mx::subst(f, x, mx::Expr(5)).str() << '\n';
        std::cout << "expand((x+1)^3)  = "
                  << mx::expand(pow(x + 1, 3)).str() << '\n';
        std::cout << "factor(x^2-1)    = "
                  << mx::factor(pow(mx::Expr(x), 2) - 1).str() << '\n';

        // Exact arithmetic: not 0.7333...
        std::cout << "1/3 + 2/5        = "
                  << (mx::Expr(1) / mx::Expr(3) + mx::Expr(2) / mx::Expr(5)).str()
                  << '\n';

        // An integral, and the derivative of the result to check it.
        const mx::Expr integrand = pow(mx::Expr(x), 2) * mx::sin(x);
        if (const auto integral = mx::integrate(integrand, x)) {
            std::cout << "int x^2 sin(x)   = " << integral->str() << '\n';
            std::cout << "  differentiated = "
                      << mx::ratsimp(mx::diff(*integral, x)).str() << '\n';
        }

        if (const auto area = mx::integrate(x * x, x, mx::Expr(0), mx::Expr(1))) {
            std::cout << "int_0^1 x^2      = " << area->str() << '\n';
        }

        if (const auto l = mx::limit(mx::sin(x) / x, x, mx::Expr(0))) {
            std::cout << "lim sin(x)/x     = " << l->str() << '\n';
        }

        if (const auto roots = mx::solve(eq(pow(mx::Expr(x), 2), mx::Expr(1)), x)) {
            std::cout << "solve x^2 = 1    = ";
            for (const mx::Expr &root : *roots) {
                std::cout << root.str() << ' ';
            }
            std::cout << '\n';
        }

        // A system. Values come back in the order the unknowns were asked for.
        const mx::Symbol y("y");
        const std::vector<mx::Expr> system{eq(x + y, mx::Expr(3)),
                                           eq(x - y, mx::Expr(1))};
        const std::vector<mx::Symbol> unknowns{x, y};
        if (const auto found = mx::solve(system, unknowns)) {
            for (const mx::Solution &solution : *found) {
                std::cout << "x+y=3, x-y=1     = x = " << solution[0].str()
                          << ", y = " << solution[1].str() << '\n';
            }
        }

        // Rendering. str() is one renderer among several. TeX and MathML ship
        // with the library; text2d::Renderer does not — it is a plain struct
        // in examples/text2d.hpp that inherits nothing and that the library
        // has never heard of. The same expression goes through all four, and
        // the decisions about where brackets go are shared by every one.
        const mx::Symbol a("a");
        const mx::Symbol b("b");
        const mx::Symbol c("c");
        if (const auto quadratic
                = mx::solve(eq(a * pow(mx::Expr(x), 2) + b * x + c, mx::Expr(0)), x);
            quadratic && !quadratic->empty()) {
            showRendered("a root of a*x^2 + b*x + c = 0:", quadratic->back());
        }
        showRendered("d/dx sin(x)/x:", mx::diff(mx::sin(x) / x, x));
        std::cout << '\n';

        // The other parser hands the text to Maxima itself, which accepts
        // everything its own syntax allows — but evaluates as it reads, so the
        // two answer differently.
        std::cout << "Expr::parse(5!)  = " << mx::Expr::parse("5!").str()
                  << "   (parsed, not evaluated)\n";
        if (const auto viaMaxima = mx::parse("5!")) {
            std::cout << "mx::parse(5!)    = " << viaMaxima->str()
                      << "            (Maxima evaluates as it parses)\n";
        }

        // Failure is an ordinary outcome, reported rather than thrown: Maxima
        // has no closed form for this one.
        const auto hopeless = mx::integrate(mx::exp(mx::sin(x)), x);
        std::cout << "int e^sin(x)     = "
                  << (hopeless ? hopeless->str()
                               : "no result: " + hopeless.error().message)
                  << '\n';

        // Some results depend on facts Maxima has not been told. Rather than
        // asking — impossible over a pipe — it says which fact is missing.
        const mx::Symbol n("n");
        const mx::Expr power = pow(mx::Expr(x), mx::Expr(n));

        const auto unknown = mx::integrate(power, x);
        std::cout << "int x^n          = "
                  << (unknown ? unknown->str()
                              : "no result: " + unknown.error().message)
                  << '\n';

        // Supplying it in a scope, which is discarded on the way out.
        mx::Context assuming;
        assuming.assume(gt(mx::Expr(n), mx::Expr(0)));
        if (const auto known = mx::integrate(power, x)) {
            std::cout << "  assuming n > 0 = " << known->str() << '\n';
        }

        // Once a closed form exists, turning it into numbers is ordinary
        // arithmetic. No further round trips, so this is usable in a loop.
        if (const auto antiderivative = mx::integrate(integrand, x)) {
            const auto F = mx::asFunction(*antiderivative, x);
            std::cout << "F(1) - F(0)      = " << F(1.0) - F(0.0) << '\n';
        }
    } catch (const std::exception &e) {
        std::cerr << "Maxima call failed: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
