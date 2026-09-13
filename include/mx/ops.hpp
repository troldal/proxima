#pragma once

#include <mx/expr.hpp>
#include <mx/kernel.hpp>
#include <mx/symbol.hpp>
#include <mx/traverse.hpp> // contains and replace, which need no kernel.

#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mx {

/// Why an operation produced no result.
///
/// An ordinary outcome, not a malfunction: Maxima genuinely cannot integrate
/// every integrand or solve every equation. Infrastructure failures — the
/// kernel died, nothing answered in time — throw mx::KernelError instead, and
/// operations with no ordinary failure mode throw mx::MaximaError.
struct Failure {
    /// Human-readable, and usually Maxima's own wording.
    std::string message;
};

/// The process-wide kernel, started on first use and shut down at exit.
///
/// Convenient rather than obligatory: every operation below takes a Kernel to
/// use, defaulting to this one. Safe to use from several threads, like any
/// Kernel: starting it on first use is thread-safe, and calls on it take turns.
///
/// Shut down during static destruction, like any function-local static. An
/// mx::Context still open on it by then — one with static storage duration,
/// say — does not call into the destroyed kernel: its operations throw
/// mx::KernelError, and its destructor does nothing.
Kernel &sharedKernel();

/// Parses Maxima source into an expression.
///
/// Delegates to Maxima's own parser, so the accepted syntax cannot drift from
/// the backend's — but it does need a running kernel. A free function rather
/// than Expr::parse, because Expr belongs to a layer that knows nothing about
/// the kernel.
std::expected<Expr, Failure> parse(std::string_view source,
                                   Kernel &kernel = sharedKernel());

// --- Operations with no ordinary way to fail ------------------------------
//
// These throw mx::MaximaError if Maxima objects, because there is no sensible
// mathematical reason for them to.

/// Differentiates `expr` with respect to `wrt`, `order` times.
Expr diff(const Expr &expr, const Symbol &wrt, unsigned order = 1,
          Kernel &kernel = sharedKernel());

Expr expand(const Expr &expr, Kernel &kernel = sharedKernel());
Expr factor(const Expr &expr, Kernel &kernel = sharedKernel());

/// Maxima's `ratsimp`: puts the expression over a common denominator and
/// cancels. Not a general-purpose "make it nicer", which no CAS has.
Expr simplify(const Expr &expr, Kernel &kernel = sharedKernel());

/// Substitutes `value` for every occurrence of `symbol`, in Maxima, which
/// evaluates the result: `sin(x)` with x = 0 comes back as 0. For a rewrite
/// that needs no kernel and does not evaluate, see mx::replace.
Expr subst(const Expr &expr, const Symbol &symbol, const Expr &value,
           Kernel &kernel = sharedKernel());

// --- Operations that can ordinarily fail ----------------------------------

/// Indefinite integral.
///
/// Reports a Failure when no closed form exists. Note that Maxima does not
/// treat that as an error: it returns the integral unevaluated, and that noun
/// form is what this recognises.
std::expected<Expr, Failure> integrate(const Expr &expr, const Symbol &wrt,
                                       Kernel &kernel = sharedKernel());

/// Definite integral over [from, to].
std::expected<Expr, Failure> integrate(const Expr &expr, const Symbol &wrt,
                                       const Expr &from, const Expr &to,
                                       Kernel &kernel = sharedKernel());

/// Which side to approach from, for a limit that differs either way.
enum class Side { Both, FromAbove, FromBelow };

/// The limit of `expr` as `wrt` approaches `to`.
///
/// A limit that exists comes back as its value, and so does an infinite one:
/// `inf`, `minf`, or `infinity` — Maxima's complex infinity, unbounded with no
/// direction, as for 1/x at 0 approached from both sides.
///
/// A Failure when there is no limit, whichever way Maxima says so: `und` (the
/// expression is undefined there), `ind` (it stays bounded but never settles,
/// like sin(1/x) at 0, or abs(x)/x, which is 1 on one side and -1 on the
/// other), or the limit left unevaluated because Maxima could not decide.
/// Approaching from one side can turn a Failure into a value.
std::expected<Expr, Failure> limit(const Expr &expr, const Symbol &wrt,
                                   const Expr &to, Side side = Side::Both,
                                   Kernel &kernel = sharedKernel());

/// Solves `equation` for `unknown`, returning one value per solution.
///
/// Reports a Failure when Maxima does not actually solve it. It signals that
/// not by erroring but by handing back something that is not a solution — an
/// equation still mentioning the unknown on both sides (`[x = sin(x)]`), or one
/// it never rearranged at all (`[0 = x^5-x-1]`). Both are rejected here, so a
/// success really is a solution.
///
/// An empty result means Maxima found no solutions, which is itself an answer.
std::expected<std::vector<Expr>, Failure>
solve(const Expr &equation, const Symbol &unknown,
      Kernel &kernel = sharedKernel());

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
/// Reports a Failure when the result is not a set of solutions: an equation
/// still mentioning an unknown on its right-hand side, or a solution that does
/// not give a value for every unknown asked about. An empty result means no
/// solutions exist, which is an answer rather than a failure.
///
/// An underdetermined system solves parametrically, with free parameters
/// appearing as symbols named `%r1`, `%r2` and so on — Maxima's own spelling.
/// Those are values like any other, but they are not among the unknowns, so a
/// caller wanting only fully determined solutions should check for them.
std::expected<std::vector<Solution>, Failure>
solve(std::span<const Expr> equations, std::span<const Symbol> unknowns,
      Kernel &kernel = sharedKernel());

} // namespace mx
