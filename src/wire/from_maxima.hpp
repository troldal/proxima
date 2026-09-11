#pragma once

#include "wire/sexpr.hpp"

#include <mx/expr.hpp>

#include <string>
#include <string_view>

namespace mx::detail {

/// Maps Maxima's internal representation onto an Expr.
///
/// The inbound half of the translation layer; Expr::str() is the outbound half.
/// Pure: no kernel, no I/O, so it is testable against recorded forms alone.
///
/// Anything not modelled by a typed node becomes `Function(head, args)`, and
/// anything that is not an application at all becomes `Opaque`. Between them
/// this never fails to produce *something* — a Maxima result is never
/// unrepresentable — though see the header notes on which shapes survive a
/// round trip back to Maxima unchanged.
///
/// Throws ParseError only for input that is not a well-formed Maxima term at
/// all, such as an empty list where a head was expected.
Expr fromMaxima(const SExpr &form);

/// Undoes Maxima's case inversion on a symbol name.
///
/// Maxima stores `x` as `$X` and `X` as `$x`, inverting the case of names that
/// are uniformly cased and leaving mixed-case names (`$xY`) alone. Decoding is
/// the same inversion applied again, which is why it is its own inverse.
///
/// Exposed for testing.
std::string decodeMaximaName(std::string_view raw);

} // namespace mx::detail
