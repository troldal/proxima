#pragma once

#include <proxima/expr.hpp>

#include <string>

namespace proxima {

/// @addtogroup rendering
/// @{

/// Renders an expression as LaTeX, for a maths environment: `\frac{1+x}{x-1}`,
/// `x^{2}`, `\sqrt{x}`, `\sin\left(x\right)`.
///
/// Locally, with no kernel. Maxima has a `tex()` of its own, and it is
/// reachable through Kernel::eval for anyone who wants it, but it costs a round
/// trip per render and only works on expressions that have been through Maxima.
/// This works on any Expr.
///
/// Maxima's constants are set as symbols (`%pi` as `\pi`, `inf` as `\infty`),
/// and so are plain symbols named after Greek letters: a symbol called `mu`
/// is set as `\mu`, and one called `gamma` as `\gamma`. That is what a
/// formula written in physics notation wants; if `mu` is just a name, give it
/// another one.
///
/// Written as an ordinary user of proxima/render.hpp — a plain struct, inheriting
/// nothing — so src/render/tex.cpp doubles as a worked example of supplying your
/// own renderer.
std::string to_tex(const Expr &expr);

/// @}

} // namespace proxima
