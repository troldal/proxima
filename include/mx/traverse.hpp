#pragma once

#include <mx/expr.hpp>
#include <mx/symbol.hpp>

namespace mx {

// Local operations on the expression tree: no kernel, no round trip.

/// True when `symbol` occurs anywhere in `expr`.
///
/// Opaque nodes are looked inside too, since unmodelled Maxima text can still
/// name a symbol. The text is not parsed: the name counts where it stands as a
/// whole identifier outside string literals, or — for a name that is not a
/// plain identifier, such as `x y` — wherever it appears with Maxima's
/// backslash escapes removed. Inside Opaque text it errs towards true.
bool contains(const Expr &expr, const Symbol &symbol);

/// `expr` with every occurrence of `symbol` replaced by `value`: a tree
/// rewrite, where mx::subst asks Maxima.
///
/// The result is built the way any expression is, so it is normalised —
/// numbers fold (`3*x + 2` with x = 2 is 8) and identities go (`x*y` with
/// y = 1 is x) — but it is not evaluated: `sin(x)` with x = 0 is `sin(0)`, and
/// a power of numbers such as `2^2` stands as written. Evaluating is Maxima's
/// work, or evalNumeric's and Compiled's, which take the result as it is.
/// Parts of the tree that do not mention `symbol` are shared, not copied.
///
/// A function whose head has the symbol's name is left alone: a head is a
/// name, not the symbol. An Opaque node, Maxima text this library never
/// parsed, cannot be rewritten without parsing it, so one that mentions
/// `symbol` throws mx::Error rather than leave the symbol silently behind;
/// mx::subst handles those.
Expr replace(const Expr &expr, const Symbol &symbol, const Expr &value);

} // namespace mx
