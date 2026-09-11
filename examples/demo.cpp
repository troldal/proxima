// The point of the library, in one file: symbolic mathematics through a pure
// C++ interface, with Maxima doing the work out of sight. There is no Maxima
// syntax below, and no strings standing in for expressions.

#include <mx/context.hpp>
#include <mx/errors.hpp>
#include <mx/expr.hpp>
#include <mx/functions.hpp>
#include <mx/numeric.hpp>
#include <mx/ops.hpp>
#include <mx/symbol.hpp>

#include <exception>
#include <iostream>

int main() {
    try {
        const mx::Symbol x("x");

        const mx::Expr f = pow(mx::Expr(x), 2) + 3 * x + 2;
        std::cout << "f                = " << f.str() << '\n';
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
                      << mx::simplify(mx::diff(*integral, x)).str() << '\n';
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
