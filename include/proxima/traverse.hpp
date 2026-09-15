#pragma once

#include <proxima/expr.hpp>
#include <proxima/symbol.hpp>

#include <concepts>
#include <functional>
#include <type_traits>
#include <utility>
#include <vector>

namespace proxima {

// Local operations on the expression tree: no kernel, no round trip.
//
// The three generic walks come first — visit, anyOf, transform — so a caller
// never needs to write the recursion over args() again. contains and replace
// are those walks with a symbol in mind.

namespace detail {
/// `expr` rebuilt with new operands, as many as it had, through the builder
/// for its kind — so normalised exactly as a freshly built expression is.
/// A leaf, having no operands, comes back as it is.
Expr withOperands(const Expr &expr, std::vector<Expr> operands);
} // namespace detail

/// Calls `f` on every node of `expr`, a node before its operands, operands in
/// order. The nodes are the tree as built, so normalised: `x - 1` is visited as
/// the sum of -1 and x.
template <typename F>
    requires std::invocable<F &, const Expr &>
void visit(const Expr &expr, F &&f) {
    std::invoke(f, expr);
    for (const Expr &operand : expr.args()) {
        visit(operand, f);
    }
}

/// True when `predicate` holds for some node of `expr`. Asks in the order
/// visit goes, and stops at the first yes.
template <typename P>
    requires std::predicate<P &, const Expr &>
bool anyOf(const Expr &expr, P &&predicate) {
    if (std::invoke(predicate, expr)) {
        return true;
    }
    for (const Expr &operand : expr.args()) {
        if (anyOf(operand, predicate)) {
            return true;
        }
    }
    return false;
}

/// `expr` rewritten bottom-up: `f` is called on every node once its operands
/// have been rewritten, and what it returns takes the node's place. Return the
/// node unchanged to keep it.
///
/// A node whose operands changed is rebuilt through the builders before `f`
/// sees it, so every result is normalised — but nothing is evaluated: a
/// rewrite that produces `sin(0)` or `2^2` leaves exactly that. Parts of the
/// tree `f` leaves alone are shared with `expr`, not copied.
template <typename F>
    requires std::is_invocable_r_v<Expr, F &, const Expr &>
Expr transform(const Expr &expr, F &&f) {
    const std::vector<Expr> &operands = expr.args();
    if (operands.empty()) {
        return std::invoke(f, expr);
    }
    std::vector<Expr> rewritten;
    rewritten.reserve(operands.size());
    bool changed = false;
    for (const Expr &operand : operands) {
        rewritten.push_back(transform(operand, f));
        changed = changed || !detail::sameRepresentation(rewritten.back(), operand);
    }
    if (!changed) {
        return std::invoke(f, expr);
    }
    return std::invoke(f, detail::withOperands(expr, std::move(rewritten)));
}

/// True when `symbol` occurs anywhere in `expr`.
///
/// Opaque nodes are looked inside too, since unmodelled Maxima text can still
/// name a symbol. The text is not parsed: the name counts where it stands as a
/// whole identifier outside string literals, or — for a name that is not a
/// plain identifier, such as `x y` — wherever it appears with Maxima's
/// backslash escapes removed. Inside Opaque text it errs towards true.
bool contains(const Expr &expr, const Symbol &symbol);

/// `expr` with every occurrence of `symbol` replaced by `value`: a tree
/// rewrite, where proxima::subst asks Maxima.
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
/// `symbol` throws proxima::Error rather than leave the symbol silently behind;
/// proxima::subst handles those.
Expr replace(const Expr &expr, const Symbol &symbol, const Expr &value);

} // namespace proxima
