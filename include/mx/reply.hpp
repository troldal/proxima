#pragma once

#include <string>

namespace mx {

/// The outcome of one Maxima evaluation.
///
/// The two failure modes are kept apart deliberately. A *mathematical* failure
/// — no closed form, a malformed expression, division by zero — is an ordinary
/// outcome and arrives here as `ok == false` with a reason. An *infrastructure*
/// failure — the kernel died, the protocol desynchronised, nothing answered in
/// time — is genuinely exceptional and is thrown as mx::KernelError instead.
struct Reply {
    /// False when Maxima signalled an error while evaluating.
    bool ok = false;

    /// The result, as the text of Maxima's own internal s-expression, e.g.
    /// "((MTIMES SIMP) 2 $X ((%SIN SIMP) $X))". Empty when `ok` is false.
    ///
    /// Raw by design: this is the kernel's escape hatch, for a caller who
    /// wants the wire form itself. mx::toExpr reads it into an mx::Expr, and
    /// Kernel::evalExpr evaluates and reads in one step.
    std::string value;

    /// Maxima's own rendered message when `ok` is false, e.g.
    /// "integrate: variable must not be a number; found: 5". Empty on success.
    std::string reason;
};

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

} // namespace mx
