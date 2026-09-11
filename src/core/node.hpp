#pragma once

#include <mx/expr.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace mx::detail {

/// The shared, immutable representation behind an Expr.
///
/// A tagged struct rather than a std::variant or a class hierarchy. No vtable
/// and no visitor machinery, which keeps the reading of a tree — the thing this
/// library does constantly — a switch on an enum. It wastes a few bytes per
/// node on fields the kind does not use; that is a deliberate trade against the
/// recursive-variant and cross-casting complexity the alternatives bring, and
/// it is invisible through the public API, which never exposes Node. Swapping
/// the representation later breaks nothing outside src/core.
struct Node {
    Kind kind = Kind::Integer;

    /// Computed once at construction. Consistent with Expr::operator==.
    std::size_t hash = 0;

    /// Integer value, or a Rational's numerator.
    Integer integer = 0;
    /// A Rational's denominator; always positive, always coprime with integer.
    Integer denominator = 1;

    double real = 0.0;

    RelOp relOp = RelOp::Equal;

    /// Symbol name, Function head, or Opaque source text.
    std::string text;

    /// Operands, in the order the kind implies.
    std::vector<Expr> args;
};

} // namespace mx::detail
