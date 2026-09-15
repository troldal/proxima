#pragma once

#include <proxima/expr.hpp>

#include <optional>
#include <vector>

namespace proxima::detail {

/// Puts a sum's terms into canonical form: nested sums spliced in, numeric
/// terms folded into one, a zero dropped, and the rest ordered.
///
/// May return zero or one term, so the caller still collapses those cases.
std::vector<Expr> normalizeSum(std::vector<Expr> terms);

/// The same for a product, with one extra rule: any zero factor collapses the
/// whole product to zero.
std::vector<Expr> normalizeProduct(std::vector<Expr> factors);

/// The identities of exponentiation, when one applies: `x^1` is `x`, `x^0` is
/// 1, `1^n` is 1. Returns nullopt when the power must stand as written.
std::optional<Expr> normalizePower(const Expr &base, const Expr &exponent);

/// A total order over expressions: negative, zero or positive.
///
/// Deterministic and platform-independent, which rules out ordering by hash —
/// std::hash<std::string> differs between standard libraries, so a hash order
/// would make canonical form, printed output and test expectations vary by
/// platform.
///
/// Numbers first, then symbols, then compounds, which is the order Maxima's own
/// internal representation uses: `x + 1` arrives as `((MPLUS SIMP) 1 $X)`. That
/// makes normalisation a no-op on anything mapped back from Maxima, rather than
/// a reshuffle that obscures diffs.
int compareExpr(const Expr &lhs, const Expr &rhs);

} // namespace proxima::detail
