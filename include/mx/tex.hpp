#pragma once

#include <mx/expr.hpp>

#include <string>

namespace mx {

/// Renders an expression as LaTeX, for a maths environment: `\frac{1+x}{x-1}`,
/// `x^{2}`, `\sqrt{x}`, `\sin\left(x\right)`.
///
/// Locally, with no kernel. Maxima has a `tex()` of its own, and it is
/// reachable through Kernel::eval for anyone who wants it, but it costs a round
/// trip per render and only works on expressions that have been through Maxima.
/// This works on any Expr.
///
/// Written as an ordinary user of mx/render.hpp — a plain struct, inheriting
/// nothing — so src/core/tex.cpp doubles as a worked example of supplying your
/// own renderer.
std::string toTeX(const Expr &expr);

} // namespace mx
