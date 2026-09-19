#pragma once

#include <proxima/expr.hpp>
#include <proxima/kernel.hpp>
#include <proxima/numeric.hpp>
#include <proxima/result.hpp>
#include <proxima/symbol.hpp>
#include <proxima/traverse.hpp> // contains and replace, which need no kernel.

#include <complex>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace proxima {

/// @addtogroup operations
/// @{

// Every operation here returns a proxima::result: the answer, or a Failure
// saying why there is none, with a proxima::Cause a program can branch on.
// Nothing is thrown for an ordinary outcome — not by diff, which fails only
// for a malformed argument, any more than by integrate, which fails for want
// of a closed form. Callers who prefer to catch write proxima::unwrap(...)
// around the call; callers who compose write `f | ...` with FXT's adaptors.
// The one exception is proxima::KernelError, for the kernel dying or the
// protocol breaking, which is not an outcome of the mathematics.
//
// Each takes a proxima::Env last: the kernel to ask, shared_kernel() unless
// said otherwise, and the Assumptions to ask under, none unless said
// otherwise. `integrate(f, x, assuming(gt(n, 0)))` is the whole of asking
// under an assumption; nothing is set up beforehand, and nothing is left
// behind for the next call.

/// Parses Maxima source into an expression.
///
/// Delegates to Maxima's own parser, so the accepted syntax cannot drift from
/// the backend's — but it does need a running kernel. A free function rather
/// than Expr::parse, because Expr belongs to a layer that knows nothing about
/// the kernel. A Failure, with Cause::MaximaError, for text Maxima cannot read.
///
/// Maxima's `parse_string`: it reads the text and simplifies it, so `5!` is
/// 120 and `x^2^3` is x^8, but it does not evaluate it — `diff(x^2, x)` comes
/// back as that call. To have Maxima carry out text, ask the kernel:
/// `kernel.ask(Query::text("diff(x^2, x)"))` is 2*x.
result<Expr> parse(std::string_view source, const Env &env = {});

// --- Calculus and algebra ------------------------------------------------
//
// These fail only if Maxima objects to an argument (Cause::MaximaError) or
// needs a fact it has not been told (Cause::NeedsAssumption).

/// Differentiates `expr` with respect to `wrt`, `order` times.
result<Expr> diff(const Expr &expr, const Symbol &wrt, unsigned order = 1,
                  const Env &env = {});

result<Expr> expand(const Expr &expr, const Env &env = {});
result<Expr> factor(const Expr &expr, const Env &env = {});

/// Rational simplification: puts the expression over a common denominator
/// and cancels, so (x^2 - 1)/(x - 1) is x + 1. It treats a function
/// application, sin(x) or sqrt(x), as an opaque variable, so it knows no
/// identities: for those, see trigsimp, trigexpand and radcan.
result<Expr> ratsimp(const Expr &expr, const Env &env = {});

/// The same as ratsimp, under the name it used to have. No CAS has a
/// general-purpose "make it nicer", and this is not one; new code should say
/// ratsimp, which says what happens.
result<Expr> simplify(const Expr &expr, const Env &env = {});

/// Substitutes `value` for every occurrence of `symbol`, in Maxima, which
/// evaluates the result: `sin(x)` with x = 0 comes back as 0. For a rewrite
/// that needs no kernel and does not evaluate, see proxima::replace.
result<Expr> subst(const Expr &expr, const Symbol &symbol, const Expr &value,
                   const Env &env = {});

/// The Taylor expansion of `expr` in `wrt` about `at`, up to `wrt^order`, as
/// an ordinary expression: `taylor(sin(x), x, 0, 5)` is x - x^3/6 + x^5/120.
result<Expr> taylor(const Expr &expr, const Symbol &wrt, const Expr &at,
                    unsigned order, const Env &env = {});

/// Simplifies with the Pythagorean identities: sin(x)^2 + cos(x)^2 is 1.
result<Expr> trigsimp(const Expr &expr, const Env &env = {});

/// Expands functions of sums and multiples: sin(2*x) is 2*cos(x)*sin(x).
result<Expr> trigexpand(const Expr &expr, const Env &env = {});

/// Simplifies logarithms, exponentials and radicals into a canonical form:
/// exp(2*log(x)) is x^2. Treats sqrt(x^2) as x, as Maxima's radcan does.
result<Expr> radcan(const Expr &expr, const Env &env = {});

/// The partial-fraction decomposition of `expr` in `wrt`.
result<Expr> partfrac(const Expr &expr, const Symbol &wrt, const Env &env = {});

/// Maxima's `float`: every number and numeric constant in `expr` as a
/// double, symbols left alone — `%pi + x` is 3.141592653589793 + x. For
/// evaluation with no kernel, see proxima::eval_numeric. (Not `float`, which C++
/// reserves.)
result<Expr> to_float(const Expr &expr, const Env &env = {});

/// The value of `expr` at `bindings`, as eval_numeric gives it, or from Maxima
/// where eval_numeric cannot give it.
///
/// An expression eval_numeric can evaluate is evaluated locally, with no round
/// trip. One it cannot — a function it does not know, such as
/// `bessel_j(0, x)`, or Maxima text in an Opaque node — goes to Maxima, which
/// substitutes the bindings and applies `float`; its answer is then evaluated
/// as eval_numeric would. Cached, like any question, so the second time costs
/// a lookup rather than a round trip; still, for many points, it is better to
/// find a closed form Compiled can take.
///
/// A Failure, with Cause::Eval, when Maxima's answer is no number either: a
/// symbol still without a value, a complex number (see to_complex), a function
/// Maxima cannot evaluate numerically. A failure of Maxima's own, such as an
/// error for gamma at a pole, comes back as it does from any question.
result<double> to_double(const Expr &expr, const Bindings &bindings = {},
                         const Env &env = {});

/// to_double over the complex numbers: eval_complex, falling back on Maxima.
result<std::complex<double>>
to_complex(const Expr &expr, const Bindings &bindings = {}, const Env &env = {});

/// The coefficient of `term^power` in `expr`. The expression is taken as it
/// stands, not expanded first — as Maxima's coeff does — so the coefficient
/// of x in (x + 1)^2 is 0; expand first to get 2.
result<Expr> coeff(const Expr &expr, const Expr &term, int power = 1,
                   const Env &env = {});

// --- Operations with an answer of their own for "no" ----------------------

/// Indefinite integral.
///
/// Cause::NoClosedForm when none exists. Note that Maxima does not treat that
/// as an error: it returns the integral unevaluated, and that noun form is
/// what this recognises.
result<Expr> integrate(const Expr &expr, const Symbol &wrt, const Env &env = {});

/// Definite integral over [from, to].
result<Expr> integrate(const Expr &expr, const Symbol &wrt, const Expr &from,
                       const Expr &to, const Env &env = {});

/// Which side to approach from, for a limit that differs either way.
enum class Side { Both, FromAbove, FromBelow };

/// The limit of `expr` as `wrt` approaches `to`.
///
/// A limit that exists comes back as its value, and so does an infinite one:
/// `inf`, `minf`, or `infinity` — Maxima's complex infinity, unbounded with no
/// direction, as for 1/x at 0 approached from both sides.
///
/// Cause::NoLimit when there is none, whichever way Maxima says so: `und`
/// (the expression is undefined there) or `ind` (it stays bounded but never
/// settles, like sin(1/x) at 0, or abs(x)/x, which is 1 on one side and -1 on
/// the other); Cause::NoClosedForm when the limit is left unevaluated because
/// Maxima could not decide. Approaching from one side can turn a Failure into
/// a value.
result<Expr> limit(const Expr &expr, const Symbol &wrt, const Expr &to,
                   Side side = Side::Both, const Env &env = {});

/// Solves `equation` for `unknown`, returning one value per solution.
///
/// Cause::NotSolved when Maxima does not actually solve it. It signals that
/// not by erroring but by handing back something that is not a solution — an
/// equation still mentioning the unknown on both sides (`[x = sin(x)]`), or one
/// it never rearranged at all (`[0 = x^5-x-1]`). Both are rejected here, so a
/// success really is a solution.
///
/// An empty result means Maxima found no solutions, which is itself an answer.
result<std::vector<Expr>> solve(const Expr &equation, const Symbol &unknown,
                                const Env &env = {});

/// One solution of a system: a value for each unknown, in the order they were
/// asked for, so `solution[i]` belongs to `unknowns[i]`.
///
/// The values are matched to unknowns by name rather than by the position
/// Maxima returned them in, so the correspondence holds regardless of how
/// Maxima chose to order its answer.
using Solution = std::vector<Expr>;

/// Solves a system of equations for several unknowns.
///
/// Each entry of `equations` should be a relation, as `eq(lhs, rhs)` builds.
/// A bare expression is accepted and means `expression = 0`, which is Maxima's
/// own convention.
///
/// Cause::NotSolved when the result is not a set of solutions: an equation
/// still mentioning an unknown on its right-hand side, or a solution that does
/// not give a value for every unknown asked about; Cause::Argument for no
/// equations or no unknowns. An empty result means no solutions exist, which
/// is an answer rather than a failure.
///
/// An underdetermined system solves parametrically, with free parameters
/// appearing as symbols named `%r1`, `%r2` and so on — Maxima's own spelling.
/// Those are values like any other, but they are not among the unknowns, so a
/// caller wanting only fully determined solutions should check for them.
result<std::vector<Solution>> solve(std::span<const Expr> equations,
                                    std::span<const Symbol> unknowns,
                                    const Env &env = {});

/// A differential equation's general solution, from Maxima's `ode2`, for a
/// first- or second-order ordinary equation.
///
/// Write derivatives with proxima::derivative: `eq(derivative(y, x), y)` is
/// y' = y. The answer is a relation `y = ...` holding Maxima's constants of
/// integration, `%c` for a first-order equation and `%k1`, `%k2` for a
/// second-order one. Cause::NotSolved when ode2 cannot solve it, which Maxima
/// says by answering `false`.
result<Expr> ode2(const Expr &equation, const Symbol &dependent,
                  const Symbol &independent, const Env &env = {});

/// The sum of `term` for `index` from `from` to `to`, in closed form:
/// `sum(k, k, 1, n)` is n*(n + 1)/2, and `to` may be proxima::inf().
///
/// Cause::NoClosedForm when Maxima finds none, which it says by handing back
/// the sum unevaluated. Maxima's `simpsum` is on for this evaluation only.
result<Expr> sum(const Expr &term, const Symbol &index, const Expr &from,
                 const Expr &to, const Env &env = {});

/// The product of `term` for `index` from `from` to `to`, in closed form.
/// Cause::NoClosedForm when there is none — which, for symbolic bounds, is
/// usual.
result<Expr> product(const Expr &term, const Symbol &index, const Expr &from,
                     const Expr &to, const Env &env = {});

// --- Questions under the assumptions in force ------------------------------

/// Maxima's three-valued answer to a predicate.
enum class Truth { False, True, Unknown };

/// Asks Maxima whether `predicate` holds under the Env's assumptions: under
/// `assuming(gt(a, 0))`, `is(gt(a, 0))` is True and `is(lt(a, 0))` False; with
/// nothing assumed, both are Unknown.
///
/// Cause::UnexpectedAnswer if Maxima answers anything but a truth value.
result<Truth> is(const Expr &predicate, const Env &env = {});

// --- Roots, numerically ----------------------------------------------------

/// How many distinct real roots the univariate polynomial has in the
/// half-open interval (low, high] — Sturm sequences, so the count is exact.
/// Cause::MaximaError for anything but a univariate polynomial with rational
/// coefficients.
result<std::size_t> nroots(const Expr &polynomial,
                           const Expr &low = Expr::symbol("minf"),
                           const Expr &high = Expr::symbol("inf"),
                           const Env &env = {});

/// The distinct real roots of a univariate polynomial, each once however
/// repeated, as exact rationals within Maxima's `rootsepsilon` (1e-7) of the
/// root — or exactly, when the root is rational. In the order Maxima gives
/// them, which is not sorted. Cause::MaximaError for anything but a
/// univariate polynomial with rational coefficients.
result<std::vector<Expr>> realroots(const Expr &polynomial, const Env &env = {});

/// A root of `expr` in `wrt` between `low` and `high`, found numerically by
/// Maxima's `find_root`. The expression must change sign across the interval.
///
/// Cause::MaximaError, with Maxima's message, when it does not; and
/// Cause::Eval when the expression does not evaluate to a number there,
/// because some other symbol is in it.
result<double> find_root(const Expr &expr, const Symbol &wrt, double low,
                         double high, const Env &env = {});

/// @}

} // namespace proxima
