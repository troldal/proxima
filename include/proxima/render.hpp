#pragma once

#include <proxima/expr.hpp>

#include <fxt/utils/Overload.hpp>

#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <new>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace proxima {

/// @addtogroup rendering
/// @{

/// How tightly a rendered construct binds, and how tightly a child must bind
/// to avoid being grouped. Ordinary precedence, with two extra rungs: `Atom`
/// for anything that carries its own delimiters, and `PowerBase` for the left
/// operand of a right-associative power, which needs grouping even against
/// another power — `(x^2)^3` is not `x^2^3`.
enum class Strength : int {
    Loosest = 0,
    Relation = 10,
    Sum = 20,
    Product = 30,
    Power = 40,
    PowerBase = 45,
    Atom = 50,
};

/// The things a renderer can be asked to produce.
enum class Construct {
    Integer,
    Real,
    Symbol,
    Verbatim,
    Sum,
    Product,
    Fraction,
    Power,
    Root,
    Call,
    /// Factorial and double factorial, for a renderer with a postfix notation.
    /// One without is handed them as calls.
    Postfix,
    List,
    Relation,
};

/// The positions a rendered child can occupy. A renderer overrides
/// `context_for` to say that one of these needs no grouping — which is what
/// self-delimiting notation buys: TeX writes `\frac{1+x}{x-1}` where infix
/// text has to write `(1+x)/(x-1)`.
enum class Slot {
    SumTerm,
    /// A sum term that carries a minus, and the operand of a standalone
    /// negation. Distinct from SumTerm because it binds tighter: `x - (1 + y)`
    /// must keep its brackets, where `x + (1 + y)` need not.
    NegatedTerm,
    Factor,
    Numerator,
    Denominator,
    Base,
    Exponent,
    Radicand,
    Argument,
    RelationSide,
    /// The operand of a postfix operator: `(x^2)!`, but `x!` and `f(x)!`.
    PostfixOperand,
};

/// Binding strength for an infix notation with no delimiters of its own. The
/// starting point a renderer adjusts rather than replaces.
constexpr Strength default_strength(Construct construct) {
    switch (construct) {
    case Construct::Sum:
        return Strength::Sum;
    case Construct::Product:
    case Construct::Fraction:
        return Strength::Product;
    case Construct::Power:
        return Strength::Power;
    case Construct::Relation:
        return Strength::Relation;
    case Construct::Integer:
    case Construct::Real:
    case Construct::Symbol:
    case Construct::Verbatim:
    case Construct::Root:
    case Construct::Call:
    case Construct::List:
    case Construct::Postfix:
        break;
    }
    // Root, Call and List carry their own delimiters; the leaves are atoms
    // outright. A postfix operator binds tighter than anything else, `^`
    // included: `x!^2` is `(x!)^2` and `2^x!` is `2^(x!)`.
    return Strength::Atom;
}

/// Likewise for the context a child is rendered in.
constexpr Strength default_context(Slot slot) {
    switch (slot) {
    case Slot::SumTerm:
        return Strength::Sum;
    case Slot::NegatedTerm:
    case Slot::Factor:
    case Slot::Numerator:
        return Strength::Product;
    case Slot::Denominator:
    case Slot::Exponent:
        // `1/(a*b)` and `x^(y+1)`: anything looser than a power needs
        // grouping, since the notation itself supplies no brackets.
        return Strength::Power;
    case Slot::Base:
        return Strength::PowerBase;
    case Slot::RelationSide:
        return Strength::Sum;
    case Slot::PostfixOperand:
        // Nothing but an atom goes under a postfix operator ungrouped.
        return Strength::Atom;
    case Slot::Radicand:
    case Slot::Argument:
        break;
    }
    // Delimited by the construct itself.
    return Strength::Loosest;
}

/// One addend of a sum, with its sign already extracted so that a renderer
/// writes `a - b` rather than `a + (-b)`.
template <typename T>
struct Term {
    T value;
    bool negated = false;
};

// --- what a renderer must provide -----------------------------------------
//
// Decomposed on purpose. A single monolithic concept reports "constraint not
// satisfied" and leaves you to diff two signatures; these are named so the
// first line of the diagnostic says which group is missing.

template <typename R, typename T>
concept RendersAtoms = requires(R &r, const Integer &n) {
    { r.integer(n) } -> std::same_as<T>;
    { r.real(0.0) } -> std::same_as<T>;
    { r.symbol(std::string_view{}) } -> std::same_as<T>;
    /// Maxima source this library never interpreted. No renderer can typeset
    /// it, but every renderer has to decide what to do with it, so there is
    /// no default.
    { r.verbatim(std::string_view{}) } -> std::same_as<T>;
};

template <typename R, typename T>
concept RendersArithmetic
    = requires(R &r, const T &a, const T &b, std::span<const Term<T>> terms,
               std::span<const T> items) {
          { r.sum(terms) } -> std::same_as<T>;
          { r.product(items) } -> std::same_as<T>;
          { r.fraction(a, b) } -> std::same_as<T>;
          { r.power(a, b) } -> std::same_as<T>;
      };

template <typename R, typename T>
concept RendersApplications
    = requires(R &r, const T &a, const T &b, std::span<const T> items) {
          { r.call(std::string_view{}, items) } -> std::same_as<T>;
          { r.relation(RelOp::Equal, a, b) } -> std::same_as<T>;
      };

template <typename R, typename T>
concept RendersGrouping = requires(R &r, const T &a) {
    /// How to parenthesise. The library decides *when*; a 2-D renderer draws
    /// brackets that stretch, TeX writes `\left( \right)`.
    { r.group(a) } -> std::same_as<T>;
};

/// The complete requirement.
template <typename R, typename T>
concept RendererFor = RendersAtoms<R, T> && RendersArithmetic<R, T>
                      && RendersApplications<R, T> && RendersGrouping<R, T>;

// --- what a renderer may provide ------------------------------------------
//
// Each is detected where the concrete type is still visible, so a renderer
// that does not define one gets a synthesised version built from the
// operations it does define. Nothing is inherited and nothing is overridden.

template <typename R, typename T>
concept RendersRoot = requires(R &r, const T &a) {
    { r.root(a, 2u) } -> std::same_as<T>;
};

template <typename R, typename T>
concept RendersList = requires(R &r, std::span<const T> items) {
    { r.list(items) } -> std::same_as<T>;
};

template <typename R, typename T>
concept RendersNegation = requires(R &r, const T &a) {
    { r.negate(a) } -> std::same_as<T>;
};

/// Renders a factorial or double factorial in postfix notation, given the
/// rendered operand and the operator. A renderer without it has factorials
/// rendered as the calls they are, `factorial(x)`.
template <typename R, typename T>
concept RendersPostfix = requires(R &r, const T &a) {
    { r.postfix(a, std::string_view{}) } -> std::same_as<T>;
};

template <typename R>
concept DeclaresStrength = requires(R &r) {
    { r.strength_of(Construct::Sum) } -> std::same_as<Strength>;
};

template <typename R>
concept DeclaresContext = requires(R &r) {
    { r.context_for(Slot::Numerator) } -> std::same_as<Strength>;
};

namespace detail {
/// Sees through std::reference_wrapper, so that render(e, std::ref(tex)) can
/// still deduce the output type.
template <typename R>
struct Unwrap {
    using type = R;
};
template <typename U>
struct Unwrap<std::reference_wrapper<U>> {
    using type = U;
};
} // namespace detail

/// The output type a renderer produces, deduced from its simplest operation.
template <typename R>
using RenderResult
    = decltype(std::declval<typename detail::Unwrap<R>::type &>().integer(
        std::declval<const Integer &>()));

namespace detail {

/// The display form of an expression: what the presentation layer produces
/// and what a renderer is walked over.
///
/// Not the algebraic tree. `1/3` is a Fraction here rather than a Rational,
/// `x^(1/2)` is a Root, `a*b^-1` is a Fraction, a sum's negative terms carry a
/// flag instead of a `-1` factor, and a leading negative constant has been
/// moved so that `x - 1` does not read `-1 + x`. Getting those right is the
/// work; a renderer that received the raw tree would have to redo all of it.
///
/// In a public header because the walk below is a template on the renderer's
/// output type and so has to be visible — the same reason detail::Instruction
/// is in proxima/numeric.hpp. Nothing outside src/render builds one.
enum class DisplayKind {
    Integer,
    Real,
    Symbol,
    Verbatim,
    Sum,
    Product,
    Fraction,
    Power,
    Root,
    Call,
    List,
    Relation,
    Negate,
    /// A factorial or double factorial; `text` is the function's name.
    Postfix,
};

struct DisplayNode;

// The alternatives a display node can be, one per DisplayKind, each carrying
// exactly what that kind needs and nothing else. There used to be one struct
// with a kind beside `integer`, `real`, `text`, `rel_op`, `index`, `children`
// and `negated`, all present whatever the kind: a Symbol had an `index`, a
// Root had a `rel_op`, and which fields meant anything was a rule the walk
// had to remember.
//
// The fixed-arity kinds take their operands through a constructor, so a
// fraction with three parts or a relation with one cannot be built. They hold
// them in a vector because DisplayNode is incomplete here, as detail::Power
// holds its two; the bodies are below the node, where it is complete.

/// One addend of a sum, with its sign already pulled out, so that a renderer
/// writes `a - b` rather than `a + (-b)`.
///
/// The sign travels *with* the term. It used to be a `std::vector<bool>`
/// parallel to the children, which the walk read with `i < negated.size() &&
/// negated[i]` — a length check standing in for an invariant no type held.
struct DisplayTerm;

struct DisplayInteger {
    Integer value = 0;
};

struct DisplayReal {
    double value = 0.0;
};

struct DisplaySymbol {
    std::string name;
};

/// Maxima source this library never interpreted.
struct DisplayVerbatim {
    std::string source;
};

struct DisplaySum {
    std::vector<DisplayTerm> terms;
};

struct DisplayProduct {
    std::vector<DisplayNode> factors;
};

class DisplayFraction {
public:
    DisplayFraction(DisplayNode numerator, DisplayNode denominator);
    const DisplayNode &numerator() const;
    const DisplayNode &denominator() const;

private:
    std::vector<DisplayNode> parts_;
};

class DisplayPower {
public:
    DisplayPower(DisplayNode base, DisplayNode exponent);
    const DisplayNode &base() const;
    const DisplayNode &exponent() const;

private:
    std::vector<DisplayNode> parts_;
};

/// A root and its index: 2 for a square root.
class DisplayRoot {
public:
    DisplayRoot(DisplayNode radicand, unsigned index);
    const DisplayNode &radicand() const;
    unsigned index() const { return index_; }

private:
    std::vector<DisplayNode> parts_;
    unsigned index_;
};

struct DisplayCall {
    std::string head;
    std::vector<DisplayNode> args;
};

struct DisplayList {
    std::vector<DisplayNode> items;
};

class DisplayRelation {
public:
    DisplayRelation(RelOp op, DisplayNode lhs, DisplayNode rhs);
    RelOp op() const { return op_; }
    const DisplayNode &lhs() const;
    const DisplayNode &rhs() const;

private:
    RelOp op_;
    std::vector<DisplayNode> sides_;
};

class DisplayNegate {
public:
    explicit DisplayNegate(DisplayNode operand);
    const DisplayNode &operand() const;

private:
    std::vector<DisplayNode> operand_;
};

/// A factorial or double factorial; `name` is the function's name.
class DisplayPostfix {
public:
    DisplayPostfix(std::string name, DisplayNode operand);
    const std::string &name() const { return name_; }
    const DisplayNode &operand() const;

private:
    std::string name_;
    std::vector<DisplayNode> operand_;
};

/// The display node itself: one of the above, and nothing beside it.
struct DisplayNode {
    using Payload
        = std::variant<DisplayInteger, DisplayReal, DisplaySymbol, DisplayVerbatim,
                       DisplaySum, DisplayProduct, DisplayFraction, DisplayPower,
                       DisplayRoot, DisplayCall, DisplayList, DisplayRelation,
                       DisplayNegate, DisplayPostfix>;

    Payload payload;

    /// Which kind this is, read off the alternative held. Each alternative
    /// sits at the index its DisplayKind names — the assertions below hold
    /// that true — so this is a cast rather than a table to keep in step.
    DisplayKind kind() const { return static_cast<DisplayKind>(payload.index()); }
};

struct DisplayTerm {
    DisplayNode node;
    bool negated = false;
};

/// True when `K`'s alternative is `A`: what makes DisplayNode::kind() a cast.
template <DisplayKind K, typename A>
inline constexpr bool kAlternativeIs = std::is_same_v<
    std::variant_alternative_t<static_cast<std::size_t>(K), DisplayNode::Payload>,
    A>;

static_assert(std::variant_size_v<DisplayNode::Payload>
                  == static_cast<std::size_t>(DisplayKind::Postfix) + 1,
              "one alternative per DisplayKind");
static_assert(kAlternativeIs<DisplayKind::Integer, DisplayInteger>);
static_assert(kAlternativeIs<DisplayKind::Real, DisplayReal>);
static_assert(kAlternativeIs<DisplayKind::Symbol, DisplaySymbol>);
static_assert(kAlternativeIs<DisplayKind::Verbatim, DisplayVerbatim>);
static_assert(kAlternativeIs<DisplayKind::Sum, DisplaySum>);
static_assert(kAlternativeIs<DisplayKind::Product, DisplayProduct>);
static_assert(kAlternativeIs<DisplayKind::Fraction, DisplayFraction>);
static_assert(kAlternativeIs<DisplayKind::Power, DisplayPower>);
static_assert(kAlternativeIs<DisplayKind::Root, DisplayRoot>);
static_assert(kAlternativeIs<DisplayKind::Call, DisplayCall>);
static_assert(kAlternativeIs<DisplayKind::List, DisplayList>);
static_assert(kAlternativeIs<DisplayKind::Relation, DisplayRelation>);
static_assert(kAlternativeIs<DisplayKind::Negate, DisplayNegate>);
static_assert(kAlternativeIs<DisplayKind::Postfix, DisplayPostfix>);

// The fixed-arity alternatives' bodies, now that DisplayNode is complete.

inline DisplayFraction::DisplayFraction(DisplayNode numerator,
                                        DisplayNode denominator)
    : parts_{std::move(numerator), std::move(denominator)} {}
inline const DisplayNode &DisplayFraction::numerator() const {
    return parts_[0];
}
inline const DisplayNode &DisplayFraction::denominator() const {
    return parts_[1];
}

inline DisplayPower::DisplayPower(DisplayNode base, DisplayNode exponent)
    : parts_{std::move(base), std::move(exponent)} {}
inline const DisplayNode &DisplayPower::base() const {
    return parts_[0];
}
inline const DisplayNode &DisplayPower::exponent() const {
    return parts_[1];
}

inline DisplayRoot::DisplayRoot(DisplayNode radicand, unsigned index)
    : parts_{std::move(radicand)}, index_(index) {}
inline const DisplayNode &DisplayRoot::radicand() const {
    return parts_[0];
}

inline DisplayRelation::DisplayRelation(RelOp op, DisplayNode lhs, DisplayNode rhs)
    : op_(op), sides_{std::move(lhs), std::move(rhs)} {}
inline const DisplayNode &DisplayRelation::lhs() const {
    return sides_[0];
}
inline const DisplayNode &DisplayRelation::rhs() const {
    return sides_[1];
}

inline DisplayNegate::DisplayNegate(DisplayNode operand)
    : operand_{std::move(operand)} {}
inline const DisplayNode &DisplayNegate::operand() const {
    return operand_[0];
}

inline DisplayPostfix::DisplayPostfix(std::string name, DisplayNode operand)
    : name_(std::move(name)), operand_{std::move(operand)} {}
inline const DisplayNode &DisplayPostfix::operand() const {
    return operand_[0];
}

/// A display node holding `alternative`. The one way one is built.
template <typename A>
DisplayNode display_node(A alternative) {
    return DisplayNode{DisplayNode::Payload(std::move(alternative))};
}

/// Expr -> display form. Layer one, and the part every renderer shares.
DisplayNode to_display(const Expr &expr);

constexpr Construct construct_of(DisplayKind kind) {
    switch (kind) {
    case DisplayKind::Integer:
        return Construct::Integer;
    case DisplayKind::Real:
        return Construct::Real;
    case DisplayKind::Symbol:
        return Construct::Symbol;
    case DisplayKind::Verbatim:
        return Construct::Verbatim;
    case DisplayKind::Product:
        return Construct::Product;
    case DisplayKind::Fraction:
        return Construct::Fraction;
    case DisplayKind::Power:
        return Construct::Power;
    case DisplayKind::Root:
        return Construct::Root;
    case DisplayKind::Call:
        return Construct::Call;
    case DisplayKind::Postfix:
        return Construct::Postfix;
    case DisplayKind::List:
        return Construct::List;
    case DisplayKind::Relation:
        return Construct::Relation;
    case DisplayKind::Sum:
    case DisplayKind::Negate:
        break;
    }
    // A negation binds exactly as loosely as the sum it is a degenerate case
    // of, which is what puts brackets round `(-x)^2`.
    return Construct::Sum;
}

/// A concrete renderer with its optional operations resolved: what the walk
/// calls. Each optional operation is decided here, where the renderer's type
/// is still visible, so one that does not define it gets a version built from
/// the operations it does define. Nothing is inherited, nothing is
/// overridden, and nothing is called through a pointer.
///
/// root() is the exception: it is not synthesised here but by the walk, the
/// only place the grouping of both base and exponent can be decided — see
/// root_as_power. For a renderer without one, the walk never reaches root().
template <typename R, typename T>
struct Resolved {
    static_assert(RendersAtoms<R, T>,
                  "the renderer is missing integer(), real(), symbol() or "
                  "verbatim(), or one of them does not return exactly T");
    static_assert(RendersArithmetic<R, T>,
                  "the renderer is missing sum(), product(), fraction() or "
                  "power(), or one of them does not return exactly T");
    static_assert(RendersApplications<R, T>,
                  "the renderer is missing call() or relation(), or one of "
                  "them does not return exactly T");
    static_assert(RendersGrouping<R, T>,
                  "the renderer is missing group(), which says how to "
                  "parenthesise");

    R &renderer;

    T postfix(const T &operand, std::string_view op) {
        if constexpr (RendersPostfix<R, T>) {
            return renderer.postfix(operand, op);
        } else {
            static_cast<void>(operand);
            static_cast<void>(op);
            return renderer.verbatim(std::string_view{}); // Unreachable.
        }
    }

    T root(const T &radicand, unsigned index) {
        if constexpr (RendersRoot<R, T>) {
            return renderer.root(radicand, index);
        } else {
            static_cast<void>(radicand);
            static_cast<void>(index);
            return renderer.verbatim(std::string_view{}); // Unreachable.
        }
    }

    T list(std::span<const T> items) {
        if constexpr (RendersList<R, T>) {
            return renderer.list(items);
        } else {
            return renderer.call("list", items);
        }
    }

    T negate(const T &inner) {
        if constexpr (RendersNegation<R, T>) {
            return renderer.negate(inner);
        } else {
            // A sum of one negated term, which is what negation is; every
            // renderer already handles a leading minus there.
            const Term<T> single{inner, true};
            return renderer.sum(std::span<const Term<T>>(&single, 1));
        }
    }

    Strength strength_of(Construct construct) {
        if constexpr (DeclaresStrength<R>) {
            return renderer.strength_of(construct);
        } else {
            return default_strength(construct);
        }
    }

    Strength context_for(Slot slot) {
        if constexpr (DeclaresContext<R>) {
            return renderer.context_for(slot);
        } else {
            return default_context(slot);
        }
    }
};

/// `radicand^(1/index)`, as a display tree, for a renderer with no notation
/// for roots of its own.
///
/// Built as nodes and walked rather than assembled from already-rendered
/// text, which is the whole point. Text cannot say whether the radicand needs
/// brackets as a power base; an earlier version that tried printed
/// `sqrt(1 - x^2)` as `1 - x^2^(1/2)`, which is a different expression.
inline DisplayNode root_as_power(const DisplayRoot &root) {
    DisplayNode exponent = display_node(
        DisplayFraction(display_node(DisplayInteger{Integer(1)}),
                        display_node(DisplayInteger{Integer(root.index())})));
    return display_node(DisplayPower(root.radicand(), std::move(exponent)));
}

/// A postfix node as the call it stands for, for a renderer with no postfix
/// notation: `factorial(x)`, with its operand grouped as an argument rather
/// than as an operand.
inline DisplayNode postfix_as_call(const DisplayPostfix &postfix) {
    std::vector<DisplayNode> args;
    args.push_back(postfix.operand());
    return display_node(DisplayCall{postfix.name(), std::move(args)});
}

/// The operator a postfix node is written with.
inline std::string_view postfix_operator(const DisplayPostfix &postfix) {
    return postfix.name() == "double_factorial" ? "!!" : "!";
}

/// Layer two: the walk, with grouping applied. A template on the concrete
/// renderer, so every operation is a direct call the compiler can inline.
///
/// Generic over the output type too, which is the reason the walk is in a
/// header at all — a 2-D text renderer returns boxes with a width, height and
/// baseline, not strings, and an interface fixed to std::string would
/// foreclose exactly the renderer that needs it most.
template <typename R, typename T>
T render_node(const DisplayNode &node, Strength context, Resolved<R, T> &resolved) {
    R &renderer = resolved.renderer;

    // A root or a postfix a renderer has no notation for is replaced
    // outright by the construct it stands for, and that replacement decides
    // its own grouping — so this returns rather than falling through to the
    // grouping decision below.
    if constexpr (!RendersRoot<R, T>) {
        if (const auto *root = std::get_if<DisplayRoot>(&node.payload)) {
            return render_node(root_as_power(*root), context, resolved);
        }
    }
    if constexpr (!RendersPostfix<R, T>) {
        if (const auto *postfix = std::get_if<DisplayPostfix>(&node.payload)) {
            return render_node(postfix_as_call(*postfix), context, resolved);
        }
    }

    const auto child = [&](const DisplayNode &operand, Slot slot) {
        return render_node(operand, resolved.context_for(slot), resolved);
    };
    const auto each = [&](const std::vector<DisplayNode> &operands, Slot slot) {
        std::vector<T> rendered;
        rendered.reserve(operands.size());
        for (const DisplayNode &operand : operands) {
            rendered.push_back(child(operand, slot));
        }
        return rendered;
    };

    // One arm per alternative, each reaching only for what its own kind has.
    // A missing arm is a compile error; there is no fallthrough returning
    // something empty for a kind nobody handled.
    T value = std::visit(
        fxt::overload{
            [&](const DisplayInteger &number) {
                return renderer.integer(number.value);
            },
            [&](const DisplayReal &number) { return renderer.real(number.value); },
            [&](const DisplaySymbol &symbol) {
                return renderer.symbol(std::string_view(symbol.name));
            },
            [&](const DisplayVerbatim &verbatim) {
                return renderer.verbatim(std::string_view(verbatim.source));
            },

            [&](const DisplaySum &sum) {
                std::vector<Term<T>> terms;
                terms.reserve(sum.terms.size());
                for (const DisplayTerm &term : sum.terms) {
                    terms.push_back(
                        {child(term.node,
                               term.negated ? Slot::NegatedTerm : Slot::SumTerm),
                         term.negated});
                }
                return renderer.sum(std::span<const Term<T>>(terms));
            },

            [&](const DisplayProduct &product) {
                const std::vector<T> factors = each(product.factors, Slot::Factor);
                return renderer.product(std::span<const T>(factors));
            },

            [&](const DisplayFraction &fraction) {
                return renderer.fraction(
                    child(fraction.numerator(), Slot::Numerator),
                    child(fraction.denominator(), Slot::Denominator));
            },

            [&](const DisplayPower &power) {
                return renderer.power(child(power.base(), Slot::Base),
                                      child(power.exponent(), Slot::Exponent));
            },

            [&](const DisplayRoot &root) {
                // Only reached when the renderer has root(); see above.
                return resolved.root(child(root.radicand(), Slot::Radicand),
                                     root.index());
            },

            [&](const DisplayCall &call) {
                const std::vector<T> args = each(call.args, Slot::Argument);
                return renderer.call(std::string_view(call.head),
                                     std::span<const T>(args));
            },

            [&](const DisplayList &list) {
                const std::vector<T> items = each(list.items, Slot::Argument);
                return resolved.list(items);
            },

            [&](const DisplayRelation &relation) {
                return renderer.relation(relation.op(),
                                         child(relation.lhs(), Slot::RelationSide),
                                         child(relation.rhs(), Slot::RelationSide));
            },

            [&](const DisplayNegate &negation) {
                return resolved.negate(child(negation.operand(), Slot::NegatedTerm));
            },

            [&](const DisplayPostfix &postfix) {
                return resolved.postfix(
                    child(postfix.operand(), Slot::PostfixOperand),
                    postfix_operator(postfix));
            },
        },
        node.payload);

    if (resolved.strength_of(construct_of(node.kind())) < context) {
        return renderer.group(value);
    }
    return value;
}

/// Renders `expr` with a concrete renderer, held by reference.
template <typename T, typename R>
T render_with(const Expr &expr, R &renderer) {
    Resolved<R, T> resolved{renderer};
    return render_node(to_display(expr), Strength::Loosest, resolved);
}

/// The three things a Renderer needs of the renderer it holds: to render
/// with it, to destroy it, and to move it.
///
/// A hand-written table rather than an abstract base: the point of erasing
/// the renderer is that a user's type is a plain struct, owing nothing to
/// this library — no inheritance, no overrides, and nothing virtual in their
/// own type. It erases at the top, around the whole walk, so an erased
/// renderer pays one indirect call per expression rather than one per node.
template <typename T>
struct RendererVTable {
    T (*render)(void *, const Expr &);
    void (*destroy)(void *) noexcept;
    /// Moves the held renderer into `destination`, returning where it landed.
    void *(*relocate)(void *source, void *destination) noexcept;
};

/// How a held renderer is stored and reached. The specialisation is what
/// makes `std::ref(my_renderer)` work: a reference wrapper stores a pointer
/// and owns nothing, so a renderer accumulating state stays reachable.
template <typename R>
struct Model {
    using Stored = R;
    static R &get(void *p) { return *static_cast<R *>(p); }
    /// A forwarding reference, so a renderer can be handed over as a named
    /// lvalue (copied) or a temporary (moved) alike.
    template <typename V>
    static void put(void *p, V &&value) {
        new (p) R(std::forward<V>(value));
    }
};

template <typename U>
struct Model<std::reference_wrapper<U>> {
    using Stored = U *;
    static U &get(void *p) { return **static_cast<U **>(p); }
    template <typename V>
    static void put(void *p, V &&value) {
        new (p) U *(&value.get());
    }
};

inline constexpr std::size_t kRendererStorage = 6 * sizeof(void *);

} // namespace detail

/// Renders `expr` with anything that satisfies RendererFor, deducing the
/// output type from the renderer itself: `proxima::render(e, TeX{})`.
///
/// Walks the concrete renderer directly; nothing is erased. A renderer passed
/// as a named object is copied, and the copy renders — pass `std::ref(yours)`
/// to render with your own object and read its state afterwards.
template <typename R, typename T = RenderResult<std::remove_cvref_t<R>>>
T render(const Expr &expr, R &&renderer) {
    using Bare = std::remove_cvref_t<R>;
    using Actual = typename detail::Unwrap<Bare>::type;
    if constexpr (!std::same_as<Actual, Bare>) {
        return detail::render_with<T>(expr, renderer.get());
    } else if constexpr (std::is_lvalue_reference_v<R>) {
        Bare copy(renderer);
        return detail::render_with<T>(expr, copy);
    } else {
        // A temporary, rendered with in place.
        return detail::render_with<T>(expr, renderer);
    }
}

/// A renderer, as a value: one type, whatever renderer producing T it holds.
///
/// Only needed to choose a renderer at run time or to keep several in one
/// container; render() on a concrete renderer needs none of this. Built on
/// the same walk, instantiated for the held renderer when it is adopted, so
/// holding one costs an indirect call per expression and nothing per node.
/// Small renderers — which is nearly all of them, since most are stateless or
/// hold a little configuration — are stored inline and never allocate.
///
/// Move-only. Requiring copyability would constrain every user's type for a
/// capability almost nothing wants; it is the wart std::move_only_function
/// exists to avoid. Wrap in std::ref to keep your own object and read its
/// state afterwards:
///
///     TeXRenderer tex;
///     proxima::Renderer<std::string> held = std::ref(tex);
///     const std::string out = proxima::render(expr, held);
///     for (const std::string &package : tex.packages_used()) { ... }
template <typename T>
class Renderer {
public:
    template <typename R, typename Bare = std::remove_cvref_t<R>>
        requires(!std::same_as<Bare, Renderer>)
    Renderer(R &&renderer) { // NOLINT: implicit on purpose
        adopt(std::forward<R>(renderer));
    }

    ~Renderer() {
        if (object_ != nullptr) {
            vtable_->destroy(object_);
        }
    }

    Renderer(Renderer &&other) noexcept : vtable_(other.vtable_) {
        if (other.object_ != nullptr) {
            object_ = vtable_->relocate(other.object_, storage_);
            other.object_ = nullptr;
        }
    }

    Renderer &operator=(Renderer &&other) noexcept {
        if (this != &other) {
            if (object_ != nullptr) {
                vtable_->destroy(object_);
                object_ = nullptr;
            }
            vtable_ = other.vtable_;
            if (other.object_ != nullptr) {
                object_ = vtable_->relocate(other.object_, storage_);
                other.object_ = nullptr;
            }
        }
        return *this;
    }

    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;

    /// Renders `expr` with the held renderer.
    T render(const Expr &expr) { return vtable_->render(object_, expr); }

private:
    template <typename R>
    void adopt(R &&renderer) {
        using Bare = std::remove_cvref_t<R>;
        using M = detail::Model<Bare>;
        using Stored = typename M::Stored;

        constexpr bool kInlineStorage
            = sizeof(Stored) <= detail::kRendererStorage
              && alignof(Stored) <= alignof(std::max_align_t)
              && std::is_nothrow_move_constructible_v<Stored>;

        vtable_ = &vtable_for<Bare, kInlineStorage>();
        if constexpr (kInlineStorage) {
            M::put(storage_, std::forward<R>(renderer));
            object_ = storage_;
        } else {
            void *raw
                = ::operator new(sizeof(Stored), std::align_val_t{alignof(Stored)});
            M::put(raw, std::forward<R>(renderer));
            object_ = raw;
        }
    }

    /// The table for one concrete renderer. Its render entry instantiates the
    /// walk for that renderer, which is where a renderer missing a required
    /// operation is reported, by Resolved's assertions.
    template <typename R, bool Inline>
    static const detail::RendererVTable<T> &vtable_for() {
        using M = detail::Model<R>;
        using Stored = typename M::Stored;

        static constexpr detail::RendererVTable<T> kTable = {
            [](void *p, const Expr &expr) -> T {
                return detail::render_with<T>(expr, M::get(p));
            },
            [](void *p) noexcept {
                static_cast<Stored *>(p)->~Stored();
                if constexpr (!Inline) {
                    ::operator delete(p, std::align_val_t{alignof(Stored)});
                }
            },
            [](void *source, void *destination) noexcept -> void * {
                if constexpr (Inline) {
                    auto *from = static_cast<Stored *>(source);
                    new (destination) Stored(std::move(*from));
                    from->~Stored();
                    return destination;
                } else {
                    static_cast<void>(destination);
                    return source; // Heap: only the pointer moves.
                }
            },
        };
        return kTable;
    }

    alignas(std::max_align_t) std::byte storage_[detail::kRendererStorage]{};
    void *object_ = nullptr;
    const detail::RendererVTable<T> *vtable_ = nullptr;
};

/// Renders `expr` with an already-erased renderer.
template <typename T>
T render(const Expr &expr, Renderer<T> &renderer) {
    return renderer.render(expr);
}

/// @}

} // namespace proxima
