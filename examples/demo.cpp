#include <mx/kernel.hpp>

#include <exception>
#include <iostream>
#include <string>
#include <vector>

int main() {
    // Everything symbolic is done by Maxima. What comes back now is the text of
    // Maxima's own internal s-expression rather than its display output —
    // unambiguous, with exact rationals intact and no precedence to re-derive.
    // Turning that into a tree is the job of the term layer (see PLAN.md).
    static const std::vector<std::string> expressions = {
        "diff(x^2 + 3*x + 2, x)",
        "expand((x + 1)^3)",
        "subst(5, x, x^2 + 3*x + 2)",
        "1/3 + 2/5",
        "integrate(x^2*sin(x), x)",
        // A failure: an ordinary outcome, reported rather than thrown.
        "integrate(x, 5)",
    };

    try {
        mx::Kernel maxima;
        for (const std::string &expression : expressions) {
            const mx::Reply reply = maxima.eval(expression);
            std::cout << expression << "\n    "
                      << (reply.ok ? reply.value : "FAILED: " + reply.reason)
                      << std::endl;
        }
    } catch (const std::exception &e) {
        std::cerr << "Maxima call failed: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
