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
    /// Text only for now: PLAN.md step 7 adds the reader that turns this into a
    /// tree, and step 9 maps that tree onto mx::Expr. Until then this is the
    /// rawest useful thing to hand back.
    std::string value;

    /// Maxima's own rendered message when `ok` is false, e.g.
    /// "integrate: variable must not be a number; found: 5". Empty on success.
    std::string reason;
};

} // namespace mx
