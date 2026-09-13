#pragma once

#include <mx/expr.hpp>

#include <string>

namespace mx {

/// Renders an expression as Presentation MathML: a complete
/// `<math xmlns="http://www.w3.org/1998/Math/MathML">` element, ready to
/// embed in HTML, where every current browser typesets it natively.
///
/// Local, with no kernel, like toTeX(). Structure maps directly: a fraction is
/// `<mfrac>`, a power `<msup>`, a square root `<msqrt>`. Brackets appear only
/// where the notation itself would be ambiguous without them — round the base
/// of a power, for instance — and are `<mo>` elements, so a browser stretches
/// them to fit.
///
/// Output is ASCII: symbols such as the minus sign, pi and infinity are
/// written as numeric character references, so it survives any encoding.
///
/// Written as an ordinary user of mx/render.hpp, like toTeX(); see
/// src/core/mathml.cpp.
std::string toMathML(const Expr &expr);

} // namespace mx
