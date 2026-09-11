#include <mx/kernel.hpp>

#include <exception>
#include <iostream>
#include <string>
#include <vector>

int main() {
    // Everything symbolic is done by Maxima; this demo just drives the session
    // and prints the result text verbatim. Interpreting that text as a
    // structured expression is the job of the term layer (see PLAN.md).
    static const std::vector<std::string> statements = {
        "diff(x^2 + 3*x + 2, x);",
        "expand((x + 1)^3);",
        "subst(5, x, x^2 + 3*x + 2);",
        "subst([x = 1, y = 2], x^2 + 2*x*y + y^2);",
        "integrate(x^2*sin(x), x);",
        // Differentiating Maxima's own antiderivative should recover the
        // original integrand.
        "trigsimp(diff(integrate(x^2*sin(x), x), x));",
    };

    try {
        mx::Kernel maxima;
        for (const std::string &statement : statements) {
            std::cout << statement << "\n    " << maxima.evalRaw(statement)
                      << std::endl;
        }
    } catch (const std::exception &e) {
        std::cerr << "Maxima call failed: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
