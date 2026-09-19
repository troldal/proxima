// A program using Proxima from outside its source tree: every public header,
// both halves of the library, and FXT's adaptors, which the installed tree
// carries. Needs no Maxima, so that CI can run it anywhere: nothing here asks
// a kernel a question, but the kernel side is compiled in and linked all the
// same. Exits non-zero, saying why, if anything is not as it should be.

#include <proxima/assumptions.hpp>
#include <proxima/errors.hpp>
#include <proxima/expr.hpp>
#include <proxima/functions.hpp>
#include <proxima/kernel.hpp>
#include <proxima/mathml.hpp>
#include <proxima/numeric.hpp>
#include <proxima/ops.hpp>
#include <proxima/render.hpp>
#include <proxima/result.hpp>
#include <proxima/symbol.hpp>
#include <proxima/tex.hpp>
#include <proxima/traverse.hpp>
#include <proxima/version.hpp>

#include <fxt/monads/Transform.hpp>

#include <cmath>
#include <complex>
#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(bool ok, const char *what) {
    if (!ok) {
        std::fprintf(stderr, "consumer: FAILED: %s\n", what);
        ++failures;
    }
}

} // namespace

int main() {
    namespace px = proxima;
    const px::Symbol x("x");

    // Expressions, and the offline parser.
    const px::Expr f = pow(x, 2) + 3 * x + 2;
    check(px::Expr::parse("x^2 + 3*x + 2") == f,
          "Expr::parse agrees with the builders");
    check(f.str() == "2 + x^2 + 3*x", "str() prints the normalised form");

    // Rendering.
    check(px::to_tex(px::sqrt(x)) == "\\sqrt{x}", "to_tex");
    check(!px::to_mathml(f).empty(), "to_mathml");

    // Numeric evaluation, one-shot, compiled and complex.
    check(*px::eval_numeric(f, {{x, 1.0}}) == 6.0, "eval_numeric");
    const px::Compiled g(px::log(px::sec(x)), x);
    check(std::abs(g(1.0) + std::log(std::cos(1.0))) < 1e-12, "Compiled");
    check(*px::eval_complex(pow(px::i(), 2)) == std::complex<double>(-1, 0),
          "eval_complex");

    // The kernel side, linked in: to_double needs no Maxima for what it can
    // evaluate itself.
    check(*px::to_double(px::sin(x), {{x, 0.0}}) == 0.0, "to_double");

    // Results composed with FXT, whose headers come with Proxima's.
    const px::result<std::string> text
        = px::Expr::parse("x + 1")
          | fxt::transform([](const px::Expr &e) { return e.str(); });
    check(text && *text == "1 + x", "FXT's transform over a result");

    check(std::string(px::version).starts_with("0."), "the version header");

    if (failures == 0) {
        std::printf("consumer: Proxima %s works from outside its tree\n",
                    px::version);
    }
    return failures == 0 ? 0 : 1;
}
