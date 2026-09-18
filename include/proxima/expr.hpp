#pragma once

#include <proxima/integer.hpp>
#include <proxima/result.hpp>

#include <fxt/utils/Overload.hpp>

#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <iosfwd>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace proxima {

class Expr;
class Symbol;

/// An Expr, or a Symbol, which converts to one.
///
/// What proxima::pow insists on for at least one argument, and the builders in
/// <proxima/functions.hpp> for theirs. They share names with <cmath>, and an
/// argument of plain `const Expr &` would accept a plain number too, through
/// Expr's implicit constructor — so under `using namespace proxima`, `pow(2, 3)` or
/// `abs(-3)` would find a Proxima candidate: losing overload resolution today, and
/// an ambiguity the day someone adds an overload. With this constraint, a call
/// on plain numbers alone has no Proxima candidate at all.
template <typename T>
concept ExprArgument = std::same_as<T, Expr> || std::same_as<T, Symbol>;

/// An exact fraction, reduced, with the sign on the numerator and a positive
/// denominator coprime with it. What a Rational holds, and what
/// Expr::as_fraction hands back for any exact number.
struct Fraction {
    Integer numerator;
    Integer denominator;

    bool operator==(const Fraction &) const = default;
};

/// One view per kind of node, for Expr::match. Defined after Expr, below.
namespace node {
struct Integer;
struct Rational;
struct Real;
struct Symbol;
struct Sum;
struct Product;
struct Power;
struct Call;
struct Relation;
struct Opaque;
} // namespace node

namespace detail {
struct Node;

/// True when `Visitor` can be called with every node view: what makes a
/// match total.
template <typename Visitor>
concept HandlesEveryKind = std::invocable<Visitor &, const node::Integer &>
                           && std::invocable<Visitor &, const node::Rational &>
                           && std::invocable<Visitor &, const node::Real &>
                           && std::invocable<Visitor &, const node::Symbol &>
                           && std::invocable<Visitor &, const node::Sum &>
                           && std::invocable<Visitor &, const node::Product &>
                           && std::invocable<Visitor &, const node::Power &>
                           && std::invocable<Visitor &, const node::Call &>
                           && std::invocable<Visitor &, const node::Relation &>
                           && std::invocable<Visitor &, const node::Opaque &>;

/// Wraps a finished node in an Expr. The single point at which the shared
/// representation enters the value type, so nothing outside src/expr can build
/// one directly.
Expr make_expr(std::shared_ptr<const Node> node);

/// Whether two expressions share one representation. Implies ==, but not the
/// other way round, which is why proxima::transform asks this rather than == to
/// tell an operand came back untouched: 0.0 and -0.0 are equal, and a rewrite
/// from one to the other must not be mistaken for no change.
bool same_representation(const Expr &lhs, const Expr &rhs) noexcept;
} // namespace detail

enum class Kind {
    Integer,  ///< An exact whole number that fits in proxima::Integer.
    Rational, ///< An exact fraction, normalised, denominator positive.
    Real,     ///< An inexact double.
    Symbol,   ///< A named unknown: x, y, %pi.
    Add,      ///< A sum of two or more terms.
    Mul,      ///< A product of two or more factors.
    Pow,      ///< base^exponent.
    Function, ///< An uninterpreted application: sin(x), bessel_j(0, x), f(x).
    Relation, ///< x = 1, x > 0.
    Opaque,   ///< Anything not modelled above, kept as Maxima source text.
};

enum class RelOp { Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual };

/// An immutable symbolic expression.
///
/// A value type: copying is a pointer copy, the representation is shared and
/// never mutated, and the hash is computed once at construction. That is what
/// makes expressions cheap to pass around, usable as cache keys, and — unlike a
/// handle into the Maxima process — able to survive a kernel restart.
///
/// ## Why the node set is this small
///
/// Two escape hatches carry everything else. `Function(head, args)` is an
/// *uninterpreted* application, so `bessel_j`, `%gamma`, a matrix, a derivative
/// or a user-defined `f` all round-trip with no new C++ type. `Opaque(text)`
/// covers whatever is not an application at all. Between them, a Maxima result
/// is never unrepresentable — which is what lets the typed part stay this
/// small instead of growing to chase Maxima's whole term language.
///
/// ## Equality
///
/// `operator==` is *structural equality returning bool*, not an equation
/// builder. Use `eq(a, b)` to build `a = b`. This follows SymPy rather than
/// GiNaC, and for the same reason: an `==` that returns something other than
/// bool silently breaks `std::find`, `std::unordered_map`, and every test
/// assertion. Equations are rare enough to name explicitly; container lookups
/// are not.
///
/// Structural means exactly that: `x + 1` and `1 + x` are equal only once the
/// normaliser (docs/design.md step 10) orders their operands. Nothing here consults
/// Maxima, so `(x+1)^2` and `x^2+2*x+1` are different expressions.
class Expr {
public:
    /// The integer zero.
    Expr();

    /// Implicit from any integral type that is a number, so `x + 1` works. See
    /// proxima::IntegralNumber for which those are.
    template <typename T>
        requires IntegralNumber<T>
    Expr(T value) : Expr(make_integer(static_cast<Integer>(value))) {} // NOLINT

    /// Implicit from an exact integer of any size.
    Expr(Integer value) : Expr(make_integer(std::move(value))) {} // NOLINT

    /// Implicit from any floating-point type, producing an inexact Real. Note
    /// that `Expr(0.5)` and `Expr::rational(1, 2)` are different values, as
    /// they are to any CAS.
    ///
    /// A constrained template rather than `Expr(double)`, because a plain
    /// double constructor accepts anything with a standard conversion to
    /// double — bool and char among them. `Expr(true)` used to be the Real 1.0
    /// and `Expr('a')` the Real 97.0.
    template <std::floating_point T>
    Expr(T value) : Expr(real(static_cast<double>(value))) {} // NOLINT

    /// Deleted. bool and the character types convert to numbers in C++, but an
    /// expression built from `true` or `'a'` is a mistake, not 1 or 97.
    /// Deleting them, rather than merely leaving them unmatched, makes the
    /// error name the argument, and makes `x + true` a compile error where it
    /// used to be `1.0 + x`. A Maxima boolean is `Expr::symbol("true")`.
    template <typename T>
        requires BooleanOrCharacter<T>
    Expr(T) = delete;

    /// Reads an expression from infix text, with no kernel involved.
    ///
    /// A deliberate *subset* of Maxima's syntax: arithmetic, comparisons,
    /// function application, lists, strings. Statements — assignment,
    /// definition, quoting, non-commutative multiplication — are not here,
    /// because this parses expressions rather than programs. For those, and for
    /// anything else exotic, proxima::parse hands the text to Maxima's own parser
    /// and so cannot drift from it; the price is needing a running kernel.
    ///
    /// `**` is accepted as `^`, as Maxima accepts it. A literal too large for a
    /// double, such as `1e400`, is a ParseError that says so.
    ///
    /// Precedences are Maxima's, including the two that surprise people: `^` is
    /// right-associative, so `x^2^3` is `x^(2^3)`; and unary minus binds looser
    /// than `^`, so `-x^2` is `-(x^2)`.
    ///
    /// Exactness is preserved — `1/3` is a Rational, not 0.333…
    ///
    /// Integers of any size are read exactly — `proxima::Integer` is unbounded —
    /// so a factorial pasted in as text is a number rather than a blob.
    ///
    /// **This parses; it does not evaluate.** `Expr::parse("5!")` is
    /// `factorial(5)` and `Expr::parse("2^3")` is `2^3`, normalised but not
    /// computed. proxima::parse differs here as well as in grammar: it hands the
    /// text to Maxima, which evaluates as it reads, and answers 120 and 8.
    ///
    /// A Failure with Cause::Parse, naming the offset, for anything malformed,
    /// and with Cause::Overflow for numbers whose fold leaves a double's range.
    static result<Expr> parse(std::string_view source);

    static Expr integer(Integer value);

    /// An exact fraction, reduced, with the sign carried by the numerator.
    /// A whole result collapses to Kind::Integer. Throws proxima::Error if
    /// `denominator` is zero.
    static Expr rational(Integer numerator, Integer denominator);

    /// An inexact number.
    ///
    /// An infinity is not a Real: it becomes the symbol `inf` or `minf`, as in
    /// Maxima, which has no floating-point infinity. So it prints, and reads
    /// back, as that symbol, and does not fold: `Expr(inf) + 1.0` stays a sum.
    ///
    /// NaN is refused: throws proxima::Error. It has no meaning in Maxima and no
    /// spelling that reads back, and because it is not equal to itself it has
    /// no place in the canonical order the normaliser sorts operands by.
    ///
    /// Arithmetic on finite numbers that would overflow to infinity throws
    /// proxima::Error too, as Maxima reports a floating-point overflow:
    /// `Expr(1e308) * Expr(10.0)`, or an integer of a few hundred digits times
    /// 1.0.
    static Expr real(double value);
    static Expr symbol(std::string name);

    /// An uninterpreted application. `head` is the function's name without any
    /// Maxima sigil: "sin", not "%SIN" or "$SIN".
    static Expr function(std::string head, std::vector<Expr> args);

    static Expr relation(RelOp op, Expr lhs, Expr rhs);

    /// Maxima source text this library does not model. The last resort that
    /// keeps every result representable.
    static Expr opaque(std::string text);

    /// Builds a sum. One term returns that term; no terms returns zero.
    ///
    /// The way to build a long sum. Each `+` builds a whole new node, the
    /// value being immutable, so accumulating n terms one `+` at a time costs
    /// time quadratic in n — measured in Release, 16,000 terms take 1.2 s —
    /// where one add() over all of them takes 3.5 ms.
    static Expr add(std::vector<Expr> terms);

    /// The same, from any range of expressions — a view included:
    /// `Expr::add(xs | std::views::transform([](const Expr &x) { return x * x; }))`.
    template <std::ranges::input_range Terms>
        requires std::convertible_to<std::ranges::range_reference_t<Terms>, Expr>
    static Expr add(Terms &&terms) {
        return add(collect(std::forward<Terms>(terms)));
    }

    /// Builds a product. One factor returns that factor; none returns one.
    /// As with add(), build a long product here rather than one `*` at a time.
    static Expr mul(std::vector<Expr> factors);

    template <std::ranges::input_range Factors>
        requires std::convertible_to<std::ranges::range_reference_t<Factors>, Expr>
    static Expr mul(Factors &&factors) {
        return mul(collect(std::forward<Factors>(factors)));
    }

    static Expr pow(Expr base, Expr exponent);

    Kind kind() const;

    bool is(Kind k) const { return kind() == k; }

    /// True for Integer, Rational and Real — the leaves arithmetic can fold.
    bool is_number() const;

    /// True for a number that is negative. False for every non-number.
    bool is_negative_number() const;

    /// Calls whichever handler takes this node's view, and returns what it
    /// returns. The handlers are combined with fxt::overload, so a generic
    /// lambda `[](const auto &)` catches whatever the others do not:
    ///
    ///     const std::string what = e.match(
    ///         [](const node::Integer &n) { return n.value.to_string(); },
    ///         [](const node::Symbol &s) { return s.name; },
    ///         [](const node::Sum &s) { return std::to_string(s.terms.size()) + "
    ///         terms"; },
    ///         [](const auto &) { return std::string("something else"); });
    ///
    /// **Total.** A set of handlers that misses a kind does not compile, so a
    /// reader of a tree cannot forget one — where a `switch` on kind() falls
    /// through silently, and an accessor asked of the wrong kind throws.
    ///
    /// The views are references into this expression's shared node: nothing
    /// is copied, and they are valid for the duration of the handler. Keep the
    /// Expr, not the view. The result type is the common type of what the
    /// handlers return.
    template <typename... Handlers>
        requires detail::HandlesEveryKind<fxt::overload<std::decay_t<Handlers>...>>
    decltype(auto) match(Handlers &&...handlers) const;

    /// The value of an Integer; nothing for any other kind.
    std::optional<Integer> as_integer() const;

    /// An exact number as a fraction — an Integer as n/1 — and nothing for
    /// anything else, a Real included.
    std::optional<Fraction> as_fraction() const;

    /// The value of a Real; nothing for any other kind. An exact number is
    /// not converted: see proxima::eval_numeric for a number from anything.
    std::optional<double> as_real() const;

    /// A Symbol as the typed Symbol the calculus operations take; nothing for
    /// any other kind. Include <proxima/symbol.hpp> to use it.
    std::optional<Symbol> as_symbol() const;

    /// Accessors. Each throws proxima::Error if the expression is not of the kind it
    /// asks for, rather than returning something meaningless. For code that has
    /// already checked is(Kind::...); match and the as_ accessors above say
    /// the same without the precondition.
    Integer integer_value() const;
    Integer numerator() const;
    Integer denominator() const;
    double real_value() const;

    /// The name of a Symbol or the head of a Function.
    const std::string &name() const;

    RelOp relation_op() const;

    /// The source text of an Opaque node.
    const std::string &opaque_text() const;

    /// Operands: the terms of an Add, factors of a Mul, base and exponent of a
    /// Pow, arguments of a Function, the two sides of a Relation. Empty for
    /// every leaf.
    const std::vector<Expr> &args() const;
    std::size_t arity() const;
    const Expr &arg(std::size_t index) const;

    /// Cached at construction, and consistent with operator==.
    std::size_t hash() const;

    /// Renders as Maxima-compatible infix text, parenthesised by precedence.
    ///
    /// For people — diagnostics, logs, tests — rather than for loops: each
    /// call builds a display tree of the whole expression, a few allocations
    /// per node, and throws it away. Nothing is cached, so an expression is no
    /// larger for having been printed. Hashing, equality and the wire to
    /// Maxima never go through here.
    std::string str() const;

    bool operator==(const Expr &other) const;

private:
    static Expr make_integer(Integer value);

    template <typename Range>
    static std::vector<Expr> collect(Range &&range) {
        std::vector<Expr> items;
        if constexpr (std::ranges::sized_range<Range>) {
            items.reserve(static_cast<std::size_t>(std::ranges::size(range)));
        }
        for (auto &&item : range) {
            items.emplace_back(std::forward<decltype(item)>(item));
        }
        return items;
    }

    // References into the node, for match. Unchecked: called only once the
    // kind is known.
    const Integer &integer_ref() const;
    const Fraction &fraction_ref() const;
    double real_ref() const;
    const std::string &text_ref() const;
    RelOp relation_ref() const;

    explicit Expr(std::shared_ptr<const detail::Node> node)
        : node_(std::move(node)) {}

    friend Expr detail::make_expr(std::shared_ptr<const detail::Node> node);
    friend bool detail::same_representation(const Expr &lhs,
                                            const Expr &rhs) noexcept;

    std::shared_ptr<const detail::Node> node_;
};

namespace node {

/// What Expr::match hands each handler: the parts of one node, by reference.
/// The operand spans are the canonical order the normaliser put them in.

struct Integer {
    const proxima::Integer &value;
};

struct Rational {
    const proxima::Integer &numerator;
    const proxima::Integer
        &denominator; ///< Positive, and coprime with the numerator.
};

struct Real {
    double value;
};

struct Symbol {
    const std::string &name;
};

struct Sum {
    std::span<const Expr> terms;
};

struct Product {
    std::span<const Expr> factors;
};

struct Power {
    const Expr &base;
    const Expr &exponent;
};

/// An uninterpreted application: `sin(x)`, `f(x, y)`, `[1, 2]` (head `list`).
struct Call {
    const std::string &head;
    std::span<const Expr> args;
};

struct Relation {
    RelOp op;
    const Expr &lhs;
    const Expr &rhs;
};

/// Maxima source text this library does not model.
struct Opaque {
    const std::string &text;
};

} // namespace node

template <typename... Handlers>
    requires detail::HandlesEveryKind<fxt::overload<std::decay_t<Handlers>...>>
decltype(auto) Expr::match(Handlers &&...handlers) const {
    fxt::overload<std::decay_t<Handlers>...> visit{
        std::forward<Handlers>(handlers)...};
    using Visitor = decltype(visit);
    using Result
        = std::common_type_t<std::invoke_result_t<Visitor &, const node::Integer &>,
                             std::invoke_result_t<Visitor &, const node::Rational &>,
                             std::invoke_result_t<Visitor &, const node::Real &>,
                             std::invoke_result_t<Visitor &, const node::Symbol &>,
                             std::invoke_result_t<Visitor &, const node::Sum &>,
                             std::invoke_result_t<Visitor &, const node::Product &>,
                             std::invoke_result_t<Visitor &, const node::Power &>,
                             std::invoke_result_t<Visitor &, const node::Call &>,
                             std::invoke_result_t<Visitor &, const node::Relation &>,
                             std::invoke_result_t<Visitor &, const node::Opaque &>>;
    const std::vector<Expr> &operands = args();
    switch (kind()) {
    case Kind::Integer:
        return static_cast<Result>(visit(node::Integer{integer_ref()}));
    case Kind::Rational: {
        const Fraction &fraction = fraction_ref();
        return static_cast<Result>(
            visit(node::Rational{fraction.numerator, fraction.denominator}));
    }
    case Kind::Real:
        return static_cast<Result>(visit(node::Real{real_ref()}));
    case Kind::Symbol:
        return static_cast<Result>(visit(node::Symbol{text_ref()}));
    case Kind::Add:
        return static_cast<Result>(visit(node::Sum{operands}));
    case Kind::Mul:
        return static_cast<Result>(visit(node::Product{operands}));
    case Kind::Pow:
        return static_cast<Result>(visit(node::Power{operands[0], operands[1]}));
    case Kind::Function:
        return static_cast<Result>(visit(node::Call{text_ref(), operands}));
    case Kind::Relation:
        return static_cast<Result>(
            visit(node::Relation{relation_ref(), operands[0], operands[1]}));
    case Kind::Opaque:
        return static_cast<Result>(visit(node::Opaque{text_ref()}));
    }
    std::unreachable();
}

/// Arithmetic, normalised at once. Each builds a whole new canonical node, so
/// to accumulate many operands use Expr::add or Expr::mul instead of a loop.
///
/// With reals, grouping can change the last bits of the folded number:
/// `(a * b) * c` rounds once per step, `Expr::mul({a, b, c})` once for all
/// three. Exact numbers fold to the same value whichever way.
Expr operator+(const Expr &lhs, const Expr &rhs);
Expr operator-(const Expr &lhs, const Expr &rhs);
Expr operator*(const Expr &lhs, const Expr &rhs);

/// Division. Two exact integers give a Rational; anything else becomes
/// `lhs * rhs^-1`, which is how Maxima represents it internally too.
Expr operator/(const Expr &lhs, const Expr &rhs);

Expr operator-(const Expr &operand);
Expr operator+(const Expr &operand);

/// base^exponent. Either side may be a plain number — `pow(x, 2)` — but not
/// both: `pow(2, 3)` belongs to std::pow, and this is no candidate for it. See
/// ExprArgument.
template <typename Base, typename Exponent>
    requires(ExprArgument<Base> || ExprArgument<Exponent>)
            && std::convertible_to<const Base &, Expr>
            && std::convertible_to<const Exponent &, Expr>
Expr pow(const Base &base, const Exponent &exponent) {
    return Expr::pow(Expr(base), Expr(exponent));
}

/// Relation builders. Named rather than spelled with comparison operators, so
/// that `==` can keep its ordinary meaning — see the note on Expr.
Expr eq(Expr lhs, Expr rhs);
Expr ne(Expr lhs, Expr rhs);
Expr lt(Expr lhs, Expr rhs);
Expr le(Expr lhs, Expr rhs);
Expr gt(Expr lhs, Expr rhs);
Expr ge(Expr lhs, Expr rhs);

/// The two sides of a relation: `lhs(le(x + 1, 3))` is `x + 1`. Local, and
/// stricter than Maxima's: anything but a relation throws proxima::Error, where
/// Maxima's lhs would hand the expression back unchanged and hide the mistake.
Expr lhs(const Expr &relation);
Expr rhs(const Expr &relation);

/// Maxima's spelling of a relation operator: "=", "#", "<", "<=", ">", ">=".
std::string_view symbol_for(RelOp op);

/// The kind's name as the enumerator spells it: "Integer", "Add", "Opaque".
/// For messages, logs and test output; `std::format("{}", kind)` and
/// `out << kind` write the same.
std::string_view kind_name(Kind kind);

std::ostream &operator<<(std::ostream &out, Kind kind);

/// Writes `expr.str()`. `std::format("{}", expr)` works too; see the
/// std::formatter below for the notations it offers.
std::ostream &operator<<(std::ostream &out, const Expr &expr);

/// The canonical order over expressions: the order the normaliser sorts
/// operands by. Numbers first, by value, then symbols, then compounds.
///
/// Two expressions compare equivalent exactly when they are ==, which is what
/// an ordered container relies on. Weak rather than strong because 0.0 and
/// -0.0 are equivalent, as they are equal, yet print differently.
///
/// Deliberately not operator<. Expr converts from numbers and symbols, so
/// `x < 0` would compile and mean "sorts before" rather than build the
/// relation — which is what lt(x, 0) is for, and why == is the only
/// comparison operator Expr has.
std::weak_ordering canonical_order(const Expr &lhs, const Expr &rhs);

/// canonical_order as a less-than, for std::sort and for ordered containers:
/// `std::set<Expr, CanonicalLess>`, `std::map<Expr, T, CanonicalLess>`. It
/// takes a Symbol too, through its conversion to Expr.
///
/// Name it explicitly. std::less is not specialised for Expr or Symbol,
/// although the standard allows it, because libc++ 22 ignores such a
/// specialisation: its tree swaps std::less<T> for the transparent std::less<>,
/// which calls `<` directly, and Expr deliberately has none.
struct CanonicalLess {
    bool operator()(const Expr &lhs, const Expr &rhs) const {
        return canonical_order(lhs, rhs) < 0;
    }
};

namespace detail {

/// The notations a format spec can ask for.
enum class Notation { Infix, TeX, MathML };

/// `expr` written in `notation`: str(), to_tex() or to_mathml().
std::string notate(const Expr &expr, Notation notation);

} // namespace detail

} // namespace proxima

template <>
struct std::hash<proxima::Expr> {
    std::size_t operator()(const proxima::Expr &value) const noexcept {
        return value.hash();
    }
};

/// `std::format("{}", expr)` is `expr.str()`, and a spec can ask for another
/// notation: `{:tex}` is to_tex(), `{:mathml}` is to_mathml(). After the
/// notation, or instead of it, come the usual string options, separated from a
/// notation by a colon — `{:>30}`, `{:tex:*<40}`. An unknown notation is a
/// std::format_error, which for a constant format string means a compile error.
template <>
struct std::formatter<proxima::Expr, char> {
    constexpr auto parse(std::format_parse_context &context) {
        auto it = context.begin();
        const auto end = context.end();
        const auto starts_with = [&it, &end](std::string_view word) {
            auto cursor = it;
            for (const char c : word) {
                if (cursor == end || *cursor != c) {
                    return false;
                }
                ++cursor;
            }
            return true;
        };

        if (starts_with("tex")) {
            notation_ = proxima::detail::Notation::TeX;
            it += 3;
        } else if (starts_with("mathml")) {
            notation_ = proxima::detail::Notation::MathML;
            it += 6;
        }
        if (notation_ != proxima::detail::Notation::Infix && it != end
            && *it == ':') {
            ++it;
        }
        context.advance_to(it);
        return text_.parse(context);
    }

    auto format(const proxima::Expr &expr, std::format_context &context) const {
        const std::string written = proxima::detail::notate(expr, notation_);
        return text_.format(std::string_view(written), context);
    }

private:
    proxima::detail::Notation notation_ = proxima::detail::Notation::Infix;
    std::formatter<std::string_view, char> text_;
};

/// `std::format("{}", kind)` is proxima::kind_name(kind), with the usual string
/// options: `{:>8}`.
template <>
struct std::formatter<proxima::Kind, char> : std::formatter<std::string_view, char> {
    auto format(proxima::Kind kind, std::format_context &context) const {
        return std::formatter<std::string_view, char>::format(
            proxima::kind_name(kind), context);
    }
};
