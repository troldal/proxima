#pragma once

#include <proxima/expr.hpp>

#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <new>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace proxima {

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
        break;
    }
    // Root, Call and List carry their own delimiters; the leaves are atoms
    // outright.
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
    /// brackets that stretch, TeX writes \left( \right).
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
using RenderResult = decltype(std::declval<typename detail::Unwrap<R>::type &>()
                                  .integer(std::declval<const Integer &>()));

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
/// is in proxima/numeric.hpp. Nothing outside src/core builds one.
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
};

struct DisplayNode {
    DisplayKind kind = DisplayKind::Integer;
    Integer integer = 0;
    double real = 0.0;
    /// Symbol name, Call head, or Verbatim source.
    std::string text;
    RelOp rel_op = RelOp::Equal;
    /// Index of a Root: 2 for a square root.
    unsigned index = 0;
    std::vector<DisplayNode> children;
    /// Parallel to `children`, and only meaningful for a Sum.
    std::vector<bool> negated;
};

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

/// The operation table a Renderer forwards through.
///
/// A hand-written vtable rather than an abstract base: the point of erasing
/// the renderer is that a user's type is a plain struct, owing nothing to
/// this library — no inheritance, no overrides, and nothing virtual in their
/// own type.
template <typename T>
struct RendererVTable {
    T (*integer)(void *, const Integer &);
    T (*real)(void *, double);
    T (*symbol)(void *, std::string_view);
    T (*verbatim)(void *, std::string_view);
    T (*sum)(void *, std::span<const Term<T>>);
    T (*product)(void *, std::span<const T>);
    T (*fraction)(void *, const T &, const T &);
    T (*power)(void *, const T &, const T &);
    /// Null when the renderer defines no root(); the walk then renders the
    /// equivalent power instead.
    T (*root)(void *, const T &, unsigned);
    T (*call)(void *, std::string_view, std::span<const T>);
    T (*list)(void *, std::span<const T>);
    T (*relation)(void *, RelOp, const T &, const T &);
    T (*group)(void *, const T &);
    T (*negate)(void *, const T &);
    Strength (*strength_of)(void *, Construct);
    Strength (*context_for)(void *, Slot);
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

/// A renderer, as a value.
///
/// Constructed from any type satisfying RendererFor<R, T>; that type is
/// erased, so this is one concrete type whatever renderer it holds. Small
/// renderers — which is nearly all of them, since most are stateless or hold
/// a little configuration — are stored inline and never allocate.
///
/// Move-only. Requiring copyability would constrain every user's type for a
/// capability almost nothing wants; it is the wart std::move_only_function
/// exists to avoid. Wrap in std::ref to keep your own object and read its
/// state afterwards:
///
///     TeXRenderer tex;
///     const std::string out = proxima::render(expr, std::ref(tex));
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

    T integer(const Integer &value) { return vtable_->integer(object_, value); }
    T real(double value) { return vtable_->real(object_, value); }
    T symbol(std::string_view name) { return vtable_->symbol(object_, name); }
    T verbatim(std::string_view source) {
        return vtable_->verbatim(object_, source);
    }
    T sum(std::span<const Term<T>> terms) {
        return vtable_->sum(object_, terms);
    }
    T product(std::span<const T> factors) {
        return vtable_->product(object_, factors);
    }
    T fraction(const T &numerator, const T &denominator) {
        return vtable_->fraction(object_, numerator, denominator);
    }
    T power(const T &base, const T &exponent) {
        return vtable_->power(object_, base, exponent);
    }
    /// Only for a renderer that defines root() — see renders_roots(). One that
    /// does not has its roots rendered as powers by the walk itself, which is
    /// the only place the grouping of both base and exponent can be decided.
    T root(const T &radicand, unsigned index) {
        return vtable_->root(object_, radicand, index);
    }

    /// Whether the held renderer defines root().
    bool renders_roots() const { return vtable_->root != nullptr; }
    T call(std::string_view head, std::span<const T> args) {
        return vtable_->call(object_, head, args);
    }
    T list(std::span<const T> items) { return vtable_->list(object_, items); }
    T relation(RelOp op, const T &lhs, const T &rhs) {
        return vtable_->relation(object_, op, lhs, rhs);
    }
    T group(const T &inner) { return vtable_->group(object_, inner); }
    T negate(const T &inner) { return vtable_->negate(object_, inner); }

    Strength strength_of(Construct construct) {
        return vtable_->strength_of(object_, construct);
    }
    Strength context_for(Slot slot) { return vtable_->context_for(object_, slot); }

private:
    template <typename R>
    void adopt(R &&renderer) {
        using Bare = std::remove_cvref_t<R>;
        using M = detail::Model<Bare>;
        using Stored = typename M::Stored;
        using Actual = typename detail::Unwrap<Bare>::type;

        static_assert(
            RendersAtoms<Actual, T>,
            "the renderer is missing integer(), real(), symbol() or "
            "verbatim(), or one of them does not return exactly T");
        static_assert(
            RendersArithmetic<Actual, T>,
            "the renderer is missing sum(), product(), fraction() or power(), "
            "or one of them does not return exactly T");
        static_assert(
            RendersApplications<Actual, T>,
            "the renderer is missing call() or relation(), or one of them does "
            "not return exactly T");
        static_assert(RendersGrouping<Actual, T>,
                      "the renderer is missing group(), which says how to "
                      "parenthesise");

        constexpr bool kInlineStorage
            = sizeof(Stored) <= detail::kRendererStorage
              && alignof(Stored) <= alignof(std::max_align_t)
              && std::is_nothrow_move_constructible_v<Stored>;

        vtable_ = &vtable_for<Bare, kInlineStorage>();
        if constexpr (kInlineStorage) {
            M::put(storage_, std::forward<R>(renderer));
            object_ = storage_;
        } else {
            void *raw = ::operator new(sizeof(Stored),
                                       std::align_val_t{alignof(Stored)});
            M::put(raw, std::forward<R>(renderer));
            object_ = raw;
        }
    }

    template <typename Actual>
    static Strength strength_of_in(Actual &renderer, Construct construct) {
        if constexpr (DeclaresStrength<Actual>) {
            return renderer.strength_of(construct);
        } else {
            static_cast<void>(renderer);
            return default_strength(construct);
        }
    }

    template <typename Actual>
    static Strength context_for_in(Actual &renderer, Slot slot) {
        if constexpr (DeclaresContext<Actual>) {
            return renderer.context_for(slot);
        } else {
            static_cast<void>(renderer);
            return default_context(slot);
        }
    }

    using RootFn = T (*)(void *, const T &, unsigned);

    /// The root slot: the renderer's own function if it has one, else null.
    /// Two constrained overloads rather than a conditional, because the
    /// branch calling root() must not even be instantiated for a renderer
    /// that lacks it.
    template <typename R>
    static constexpr RootFn root_slot() {
        return nullptr;
    }

    template <typename R>
        requires RendersRoot<typename detail::Unwrap<R>::type, T>
    static constexpr RootFn root_slot() {
        return [](void *p, const T &radicand, unsigned index) -> T {
            return detail::Model<R>::get(p).root(radicand, index);
        };
    }

    /// Builds the operation table for one concrete renderer.
    ///
    /// This is where the optional operations are resolved: the concrete type
    /// is still visible here, so a `requires` test decides whether the slot
    /// gets the renderer's own function or one synthesised from the
    /// operations it does have. A renderer that never heard of roots still
    /// renders them, through its own power().
    template <typename R, bool Inline>
    static const detail::RendererVTable<T> &vtable_for() {
        using M = detail::Model<R>;
        using Stored = typename M::Stored;
        using Actual = typename detail::Unwrap<R>::type;

        static constexpr detail::RendererVTable<T> kTable = {
            [](void *p, const Integer &v) { return M::get(p).integer(v); },
            [](void *p, double v) { return M::get(p).real(v); },
            [](void *p, std::string_view n) { return M::get(p).symbol(n); },
            [](void *p, std::string_view s) { return M::get(p).verbatim(s); },
            [](void *p, std::span<const Term<T>> t) { return M::get(p).sum(t); },
            [](void *p, std::span<const T> f) { return M::get(p).product(f); },
            [](void *p, const T &n, const T &d) {
                return M::get(p).fraction(n, d);
            },
            [](void *p, const T &b, const T &e) {
                return M::get(p).power(b, e);
            },
            // Not synthesised here: see root_as_power.
            root_slot<R>(),
            [](void *p, std::string_view head, std::span<const T> args) {
                return M::get(p).call(head, args);
            },
            [](void *p, std::span<const T> items) -> T {
                if constexpr (RendersList<Actual, T>) {
                    return M::get(p).list(items);
                } else {
                    return M::get(p).call("list", items);
                }
            },
            [](void *p, RelOp op, const T &l, const T &r) {
                return M::get(p).relation(op, l, r);
            },
            [](void *p, const T &inner) { return M::get(p).group(inner); },
            [](void *p, const T &inner) -> T {
                if constexpr (RendersNegation<Actual, T>) {
                    return M::get(p).negate(inner);
                } else {
                    // A sum of one negated term, which is what negation is;
                    // every renderer already handles a leading minus there.
                    const Term<T> single{inner, true};
                    return M::get(p).sum(std::span<const Term<T>>(&single, 1));
                }
            },
            [](void *p, Construct c) { return strength_of_in(M::get(p), c); },
            [](void *p, Slot s) { return context_for_in(M::get(p), s); },
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

namespace detail {

/// `radicand^(1/index)`, as a display tree, for a renderer with no notation
/// for roots of its own.
///
/// Built as nodes and walked rather than assembled from already-rendered
/// text, which is the whole point. Text cannot say whether the radicand needs
/// brackets as a power base; an earlier version that tried printed
/// `sqrt(1 - x^2)` as `1 - x^2^(1/2)`, which is a different expression.
inline DisplayNode root_as_power(const DisplayNode &root) {
    DisplayNode one;
    one.kind = DisplayKind::Integer;
    one.integer = Integer(1);

    DisplayNode index;
    index.kind = DisplayKind::Integer;
    index.integer = Integer(root.index);

    DisplayNode exponent;
    exponent.kind = DisplayKind::Fraction;
    exponent.children.push_back(std::move(one));
    exponent.children.push_back(std::move(index));

    DisplayNode power;
    power.kind = DisplayKind::Power;
    power.children.push_back(root.children.front());
    power.children.push_back(std::move(exponent));
    return power;
}

/// Layer two: the walk, with grouping applied. Generic over the renderer's
/// output type, which is the whole reason this is a template — a 2-D text
/// renderer returns boxes with a width, height and baseline, not strings, and
/// an interface fixed to std::string would foreclose exactly the renderer
/// that needs it most.
template <typename T>
T render_node(const DisplayNode &node, Strength context, Renderer<T> &renderer) {
    if (node.kind == DisplayKind::Root && !renderer.renders_roots()) {
        return render_node(root_as_power(node), context, renderer);
    }

    const auto child = [&](const DisplayNode &operand, Slot slot) {
        return render_node(operand, renderer.context_for(slot), renderer);
    };
    const auto children = [&](Slot slot) {
        std::vector<T> rendered;
        rendered.reserve(node.children.size());
        for (const DisplayNode &operand : node.children) {
            rendered.push_back(child(operand, slot));
        }
        return rendered;
    };

    T value = [&]() -> T {
        switch (node.kind) {
        case DisplayKind::Integer:
            return renderer.integer(node.integer);
        case DisplayKind::Real:
            return renderer.real(node.real);
        case DisplayKind::Symbol:
            return renderer.symbol(node.text);
        case DisplayKind::Verbatim:
            return renderer.verbatim(node.text);

        case DisplayKind::Sum: {
            std::vector<Term<T>> terms;
            terms.reserve(node.children.size());
            for (std::size_t i = 0; i < node.children.size(); ++i) {
                const bool negated
                    = i < node.negated.size() && node.negated[i];
                terms.push_back(
                    {child(node.children[i],
                           negated ? Slot::NegatedTerm : Slot::SumTerm),
                     negated});
            }
            return renderer.sum(terms);
        }

        case DisplayKind::Negate:
            return renderer.negate(child(node.children[0], Slot::NegatedTerm));

        case DisplayKind::Product: {
            const std::vector<T> factors = children(Slot::Factor);
            return renderer.product(factors);
        }

        case DisplayKind::Fraction:
            return renderer.fraction(child(node.children[0], Slot::Numerator),
                                     child(node.children[1], Slot::Denominator));

        case DisplayKind::Power:
            return renderer.power(child(node.children[0], Slot::Base),
                                  child(node.children[1], Slot::Exponent));

        case DisplayKind::Root:
            return renderer.root(child(node.children[0], Slot::Radicand),
                                 node.index);

        case DisplayKind::Call: {
            const std::vector<T> args = children(Slot::Argument);
            return renderer.call(node.text, args);
        }

        case DisplayKind::List: {
            const std::vector<T> items = children(Slot::Argument);
            return renderer.list(items);
        }

        case DisplayKind::Relation:
            return renderer.relation(node.rel_op,
                                     child(node.children[0], Slot::RelationSide),
                                     child(node.children[1],
                                           Slot::RelationSide));
        }
        return renderer.verbatim("");
    }();

    if (renderer.strength_of(construct_of(node.kind)) < context) {
        return renderer.group(value);
    }
    return value;
}

} // namespace detail

/// Renders `expr` with an already-erased renderer.
template <typename T>
T render(const Expr &expr, Renderer<T> &renderer) {
    return detail::render_node(detail::to_display(expr), Strength::Loosest,
                              renderer);
}

/// Renders `expr` with anything that satisfies RendererFor, deducing the
/// output type from the renderer itself: `proxima::render(e, TeX{})`.
template <typename R, typename T = RenderResult<std::remove_cvref_t<R>>>
    requires(!std::same_as<std::remove_cvref_t<R>, Renderer<T>>)
T render(const Expr &expr, R &&renderer) {
    Renderer<T> erased(std::forward<R>(renderer));
    return render(expr, erased);
}

} // namespace proxima
