#pragma once

#include <mx/expr.hpp>
#include <mx/kernel.hpp>
#include <mx/symbol.hpp>

#include <expected>
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
/// use, defaulting to this one. Not thread-safe — see PLAN.md step 13.
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

/// Substitutes `value` for every occurrence of `symbol`.
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

// --- Inspection -----------------------------------------------------------

/// True when `symbol` occurs anywhere in `expr`. Local: no kernel involved.
bool contains(const Expr &expr, const Symbol &symbol);

} // namespace mx
