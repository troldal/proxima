#pragma once

#include <proxima/expr.hpp>
#include <proxima/symbol.hpp>

#include <concepts>
#include <cstddef>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace proxima {

/// @addtogroup traversal
/// @{

// Local operations on the expression tree: no kernel, no round trip, no
// state: every function here is pure.
//
// The generic walks come first, so a caller never needs to write the
// recursion over args() again. nodes() is the tree as a range, for the
// standard algorithms; fold() is the one recursion the others are cases of;
// rewrite() and transform() build a new tree bottom-up. visit and any_of are
// the two most common folds, kept by name. contains and replace are those
// walks with a symbol in mind.

namespace detail {
/// `expr` rebuilt with new operands, as many as it had, through the builder
/// for its kind — so normalised exactly as a freshly built expression is.
/// A leaf, having no operands, comes back as it is.
Expr with_operands(const Expr &expr, std::vector<Expr> operands);
} // namespace detail

/// Every node of `expr` as a range, in the order visit goes: a node before
/// its operands, operands in order. An input range over `const Expr &`, so
/// the standard algorithms and views apply:
///
///     std::ranges::any_of(nodes(e), [](const Expr &n) { return n.is(Kind::Opaque);
///     }); std::ranges::distance(nodes(e));                 // the size of the tree
///
/// Holds its own copy of `expr`, so a temporary is safe to walk. Not
/// std::generator, which libc++ does not have (as of LLVM 22): an explicit
/// stack of operand spans, with no coroutine frame to allocate.
class Nodes {
public:
    class iterator {
    public:
        using value_type = Expr;
        using difference_type = std::ptrdiff_t;

        iterator() = default;

        const Expr &operator*() const { return current_; }
        const Expr *operator->() const { return &current_; }

        iterator &operator++() {
            // The children come next, then whatever was pending. The spans
            // point into nodes the root keeps alive, so they are stable
            // however this iterator is moved.
            if (const std::vector<Expr> &operands = current_.args();
                !operands.empty()) {
                pending_.emplace_back(operands);
            }
            while (!pending_.empty() && pending_.back().empty()) {
                pending_.pop_back();
            }
            if (pending_.empty()) {
                done_ = true;
                return *this;
            }
            std::span<const Expr> &siblings = pending_.back();
            current_ = siblings.front();
            siblings = siblings.subspan(1);
            return *this;
        }
        void operator++(int) { ++*this; }

        friend bool operator==(const iterator &it, std::default_sentinel_t) {
            return it.done_;
        }

    private:
        friend class Nodes;
        explicit iterator(const Expr &root) : root_(root), current_(root) {}

        Expr root_; ///< Keeps the whole tree alive, so the spans below stay valid.
        Expr current_;
        std::vector<std::span<const Expr>> pending_;
        bool done_ = false;
    };

    explicit Nodes(Expr expr) : expr_(std::move(expr)) {}

    iterator begin() const { return iterator(expr_); }
    static std::default_sentinel_t end() { return {}; }

private:
    Expr expr_;
};

/// `expr`'s nodes as a range; see Nodes.
inline Nodes nodes(Expr expr) {
    return Nodes(std::move(expr));
}

/// Folds `expr` bottom-up into an `R`: `algebra(node, folded)` is called on
/// every node once its operands are folded, with `folded` holding their
/// results in operand order (empty for a leaf). The one recursion over the
/// tree, of which the other walks are special cases:
///
///     // Nodes in the tree.
///     fold<std::size_t>(e, [](const Expr &, std::span<const std::size_t> sizes) {
///         return std::accumulate(sizes.begin(), sizes.end(), std::size_t{1});
///     });
///
/// `R` is given explicitly, since the algebra's own signature mentions it.
/// The algebra can use Expr::match on the node to be told its kind.
template <typename R, typename Algebra>
    requires std::is_invocable_r_v<R, Algebra &, const Expr &, std::span<const R>>
R fold(const Expr &expr, Algebra &&algebra) {
    const std::vector<Expr> &operands = expr.args();
    if constexpr (std::is_same_v<R, bool>) {
        // No std::span<const bool> over a std::vector<bool>, which packs bits.
        const auto folded = std::make_unique<bool[]>(operands.size());
        for (std::size_t i = 0; i < operands.size(); ++i) {
            folded[i] = fold<R>(operands[i], algebra);
        }
        return std::invoke(algebra, expr,
                           std::span<const bool>(folded.get(), operands.size()));
    } else {
        std::vector<R> folded;
        folded.reserve(operands.size());
        for (const Expr &operand : operands) {
            folded.push_back(fold<R>(operand, algebra));
        }
        return std::invoke(algebra, expr, std::span<const R>(folded));
    }
}

namespace detail {

template <typename F>
std::optional<Expr> rewrite_changed(const Expr &expr, F &f) {
    // A vector for the new operands only once one has changed, with the
    // untouched prefix copied in then: a rewrite that touches one leaf of a
    // large tree allocates along that leaf's path, not at every node.
    const std::vector<Expr> &operands = expr.args();
    std::optional<std::vector<Expr>> rewritten;
    for (std::size_t i = 0; i < operands.size(); ++i) {
        std::optional<Expr> operand = rewrite_changed(operands[i], f);
        if (operand && !rewritten) {
            rewritten.emplace();
            rewritten->reserve(operands.size());
            rewritten->assign(operands.begin(),
                              operands.begin() + static_cast<std::ptrdiff_t>(i));
        }
        // Not `operand ? std::move(*operand) : operands[i]`: the other branch
        // is const, so the conditional would copy both ways.
        if (rewritten && operand) {
            rewritten->push_back(std::move(*operand));
        } else if (rewritten) {
            rewritten->push_back(operands[i]);
        }
    }
    if (!rewritten) {
        return std::invoke(f, expr);
    }
    Expr rebuilt = with_operands(expr, std::move(*rewritten));
    if (std::optional<Expr> replaced = std::invoke(f, rebuilt)) {
        return replaced;
    }
    return rebuilt;
}

} // namespace detail

/// `expr` rewritten bottom-up: `f` is called on every node once its operands
/// have been rewritten, and returns the node to put in its place, or nothing
/// to keep it. Saying "unchanged" with an empty optional is what lets the
/// untouched parts of the tree be shared rather than rebuilt:
///
///     // Every sin(u) becomes cos(u), the rest stays as it is.
///     rewrite(e, [](const Expr &n) -> std::optional<Expr> {
///         if (n.is(Kind::Function) && n.name() == "sin") {
///             return Expr::function("cos", {n.arg(0)});
///         }
///         return std::nullopt;
///     });
///
/// A node whose operands changed is rebuilt through the builders before `f`
/// sees it, so every result is normalised, but nothing is evaluated.
template <typename F>
    requires std::is_invocable_r_v<std::optional<Expr>, F &, const Expr &>
Expr rewrite(const Expr &expr, F &&f) {
    std::optional<Expr> rewritten = detail::rewrite_changed(expr, f);
    if (rewritten) {
        return std::move(*rewritten);
    }
    return expr;
}

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
bool any_of(const Expr &expr, P &&predicate) {
    if (std::invoke(predicate, expr)) {
        return true;
    }
    for (const Expr &operand : expr.args()) {
        if (any_of(operand, predicate)) {
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
///
/// rewrite says the same with `std::optional`, which is clearer about what
/// "unchanged" means; this form tells by whether `f` handed back the very
/// representation it was given.
template <typename F>
    requires std::is_invocable_r_v<Expr, F &, const Expr &>
Expr transform(const Expr &expr, F &&f) {
    // As in rewrite: a vector only once an operand has changed.
    const std::vector<Expr> &operands = expr.args();
    std::optional<std::vector<Expr>> rewritten;
    for (std::size_t i = 0; i < operands.size(); ++i) {
        Expr operand = transform(operands[i], f);
        if (!rewritten && !detail::same_representation(operand, operands[i])) {
            rewritten.emplace();
            rewritten->reserve(operands.size());
            rewritten->assign(operands.begin(),
                              operands.begin() + static_cast<std::ptrdiff_t>(i));
        }
        if (rewritten) {
            rewritten->push_back(std::move(operand));
        }
    }
    if (!rewritten) {
        return std::invoke(f, expr);
    }
    return std::invoke(f, detail::with_operands(expr, std::move(*rewritten)));
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
/// work, or eval_numeric's and Compiled's, which take the result as it is.
/// Parts of the tree that do not mention `symbol` are shared, not copied.
///
/// A function whose head has the symbol's name is left alone: a head is a
/// name, not the symbol. An Opaque node, Maxima text this library never
/// parsed, cannot be rewritten without parsing it, so one that mentions
/// `symbol` throws proxima::Error rather than leave the symbol silently behind;
/// proxima::subst handles those.
Expr replace(const Expr &expr, const Symbol &symbol, const Expr &value);

/// @}

} // namespace proxima
