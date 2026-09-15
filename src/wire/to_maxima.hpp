#pragma once

#include <proxima/expr.hpp>

#include <string>
#include <string_view>

namespace proxima::detail {

/// Renders an Expr as the text of a Maxima internal form — a Lisp
/// s-expression such as `((MPLUS) 1 $X)` — for the kernel's `cppread` helper
/// to read and evaluate.
///
/// The outbound half of the translation layer, and the mirror image of
/// fromMaxima. Before this existed the outbound direction was *text*:
/// Expr::str() rendered infix, Maxima re-parsed it, and any character the
/// reader disliked — a `$` in an Opaque, a space in a symbol name — failed in
/// Maxima's *reader*, before `errcatch` was ever entered, which produced no
/// frame and cost the caller the full Config::timeout. Sending the structure
/// instead of a rendering removes the reader from the path: the only text
/// Maxima ever parses is a string literal this layer escaped itself.
///
/// Heads are emitted without simplification flags (`(MPLUS)`, not
/// `(MPLUS SIMP)`), so Maxima simplifies what it is handed rather than
/// trusting it.
///
/// Throws proxima::Error for a value with no Maxima spelling, which today means
/// only a NaN.
std::string toMaxima(const Expr &expr);

/// Encodes a symbol name as Maxima stores it: `x` is `$X`, `X` is `|$x|`,
/// `%pi` is `$%PI`. The inverse of decodeMaximaName, bar-quoted wherever the
/// Lisp reader would otherwise alter or reject the name. Exposed for testing.
std::string encodeMaximaName(std::string_view name);

/// Renders `text` as a string literal with backslash escapes — the same
/// syntax for Maxima source and for the Lisp reader, which is what lets one
/// function serve both. Exposed for testing and for the session layer, which
/// wraps every request in one of these.
std::string stringLiteral(std::string_view text);

} // namespace proxima::detail
