#pragma once

#include <mx/integer.hpp>

#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <iosfwd>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace mx {

class Expr;
class Symbol;

/// An Expr, or a Symbol, which converts to one.
///
/// What mx::pow insists on for at least one argument, and the builders in
/// <mx/functions.hpp> for theirs. They share names with <cmath>, and an
/// argument of plain `const Expr &` would accept a plain number too, through
/// Expr's implicit constructor — so under `using namespace mx`, `pow(2, 3)` or
/// `abs(-3)` would find an mx candidate: losing overload resolution today, and
/// an ambiguity the day someone adds an overload. With this constraint, a call
/// on plain numbers alone has no mx candidate at all.
template <typename T>
concept ExprArgument = std::same_as<T, Expr> || std::same_as<T, Symbol>;

namespace detail {
struct Node;

/// Wraps a finished node in an Expr. The single point at which the shared
/// representation enters the value type, so nothing outside src/core can build
/// one directly.
Expr makeExpr(std::shared_ptr<const Node> node);

/// Whether two expressions share one representation. Implies ==, but not the
/// other way round, which is why mx::transform asks this rather than == to
/// tell an operand came back untouched: 0.0 and -0.0 are equal, and a rewrite
/// from one to the other must not be mistaken for no change.
bool sameRepresentation(const Expr &lhs, const Expr &rhs) noexcept;
} // namespace detail

enum class Kind {
    Integer,  ///< An exact whole number that fits in mx::Integer.
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
/// normaliser (PLAN.md step 10) orders their operands. Nothing here consults
/// Maxima, so `(x+1)^2` and `x^2+2*x+1` are different expressions.
class Expr {
public:
    /// The integer zero.
    Expr();

    /// Implicit from any integral type that is a number, so `x + 1` works. See
    /// mx::IntegralNumber for which those are.
    template <typename T>
        requires IntegralNumber<T>
    Expr(T value) : Expr(makeInteger(static_cast<Integer>(value))) {} // NOLINT

    /// Implicit from an exact integer of any size.
    Expr(Integer value) : Expr(makeInteger(std::move(value))) {} // NOLINT

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
    /// anything else exotic, mx::parse hands the text to Maxima's own parser
    /// and so cannot drift from it; the price is needing a running kernel.
    ///
    /// Precedences are Maxima's, including the two that surprise people: `^` is
    /// right-associative, so `x^2^3` is `x^(2^3)`; and unary minus binds looser
    /// than `^`, so `-x^2` is `-(x^2)`.
    ///
    /// Exactness is preserved — `1/3` is a Rational, not 0.333… — and an
    /// integer too large for mx::Integer becomes an Opaque node holding its
    ///
    /// Integers of any size are read exactly — `mx::Integer` is unbounded —
    /// so a factorial pasted in as text is a number rather than a blob.
    ///
    /// **This parses; it does not evaluate.** `Expr::parse("5!")` is
    /// `factorial(5)` and `Expr::parse("2^3")` is `2^3`, normalised but not
    /// computed. mx::parse differs here as well as in grammar: it hands the
    /// text to Maxima, which evaluates as it reads, and answers 120 and 8.
    ///
    /// Throws mx::ParseError, naming the offset, for anything malformed.
    static Expr parse(std::string_view source);

    static Expr integer(Integer value);

    /// An exact fraction, reduced, with the sign carried by the numerator.
    /// A whole result collapses to Kind::Integer. Throws mx::Error if
    /// `denominator` is zero.
    static Expr rational(Integer numerator, Integer denominator);

    /// An inexact number. Infinities are allowed, and reach Maxima as `inf`
    /// and `minf`.
    ///
    /// NaN is not: throws mx::Error. It has no meaning in Maxima and no
    /// spelling that reads back, and because it is not equal to itself it has
    /// no place in the canonical order the normaliser sorts operands by.
    /// Arithmetic that would fold to NaN — `Expr(inf) + Expr(-inf)`,
    /// `Expr(0.0) * Expr(inf)` — throws the same way.
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
    static Expr add(std::vector<Expr> terms);

    /// Builds a product. One factor returns that factor; none returns one.
    static Expr mul(std::vector<Expr> factors);

    static Expr pow(Expr base, Expr exponent);

    Kind kind() const;

    bool is(Kind k) const { return kind() == k; }

    /// True for Integer, Rational and Real — the leaves arithmetic can fold.
    bool isNumber() const;

    /// True for a number that is negative. False for every non-number.
    bool isNegativeNumber() const;

    /// Accessors. Each throws mx::Error if the expression is not of the kind it
    /// asks for, rather than returning something meaningless.
    Integer integerValue() const;
    Integer numerator() const;
    Integer denominator() const;
    double realValue() const;

    /// The name of a Symbol or the head of a Function.
    const std::string &name() const;

    RelOp relationOp() const;

    /// The source text of an Opaque node.
    const std::string &opaqueText() const;

    /// Operands: the terms of an Add, factors of a Mul, base and exponent of a
    /// Pow, arguments of a Function, the two sides of a Relation. Empty for
    /// every leaf.
    const std::vector<Expr> &args() const;
    std::size_t arity() const;
    const Expr &arg(std::size_t index) const;

    /// Cached at construction, and consistent with operator==.
    std::size_t hash() const;

    /// Renders as Maxima-compatible infix text, parenthesised by precedence.
    std::string str() const;

    bool operator==(const Expr &other) const;

private:
    static Expr makeInteger(Integer value);

    explicit Expr(std::shared_ptr<const detail::Node> node)
        : node_(std::move(node)) {}

    friend Expr detail::makeExpr(std::shared_ptr<const detail::Node> node);
    friend bool detail::sameRepresentation(const Expr &lhs, const Expr &rhs) noexcept;

    std::shared_ptr<const detail::Node> node_;
};

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
/// stricter than Maxima's: anything but a relation throws mx::Error, where
/// Maxima's lhs would hand the expression back unchanged and hide the mistake.
Expr lhs(const Expr &relation);
Expr rhs(const Expr &relation);

/// Maxima's spelling of a relation operator: "=", "#", "<", "<=", ">", ">=".
std::string_view symbolFor(RelOp op);

/// The kind's name as the enumerator spells it: "Integer", "Add", "Opaque".
/// For messages, logs and test output; `std::format("{}", kind)` and
/// `out << kind` write the same.
std::string_view kindName(Kind kind);

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
std::weak_ordering canonicalOrder(const Expr &lhs, const Expr &rhs);

/// canonicalOrder as a less-than, for std::sort and the like. std::less<Expr>
/// is this too, so std::set<Expr> and std::map<Expr, T> need no comparator.
struct CanonicalLess {
    bool operator()(const Expr &lhs, const Expr &rhs) const {
        return canonicalOrder(lhs, rhs) < 0;
    }
};

namespace detail {

/// The notations a format spec can ask for.
enum class Notation { Infix, TeX, MathML };

/// `expr` written in `notation`: str(), toTeX() or toMathML().
std::string notate(const Expr &expr, Notation notation);

} // namespace detail

} // namespace mx

template <>
struct std::hash<mx::Expr> {
    std::size_t operator()(const mx::Expr &value) const noexcept {
        return value.hash();
    }
};

/// The canonical order, so std::set<Expr> and std::map<Expr, T> need no
/// comparator. See mx::canonicalOrder.
template <>
struct std::less<mx::Expr> : mx::CanonicalLess {};

/// `std::format("{}", expr)` is `expr.str()`, and a spec can ask for another
/// notation: `{:tex}` is toTeX(), `{:mathml}` is toMathML(). After the
/// notation, or instead of it, come the usual string options, separated from a
/// notation by a colon — `{:>30}`, `{:tex:*<40}`. An unknown notation is a
/// std::format_error, which for a constant format string means a compile error.
template <>
struct std::formatter<mx::Expr, char> {
    constexpr auto parse(std::format_parse_context &context) {
        auto it = context.begin();
        const auto end = context.end();
        const auto startsWith = [&it, &end](std::string_view word) {
            auto cursor = it;
            for (const char c : word) {
                if (cursor == end || *cursor != c) {
                    return false;
                }
                ++cursor;
            }
            return true;
        };

        if (startsWith("tex")) {
            notation_ = mx::detail::Notation::TeX;
            it += 3;
        } else if (startsWith("mathml")) {
            notation_ = mx::detail::Notation::MathML;
            it += 6;
        }
        if (notation_ != mx::detail::Notation::Infix && it != end && *it == ':') {
            ++it;
        }
        context.advance_to(it);
        return text_.parse(context);
    }

    auto format(const mx::Expr &expr, std::format_context &context) const {
        const std::string written = mx::detail::notate(expr, notation_);
        return text_.format(std::string_view(written), context);
    }

private:
    mx::detail::Notation notation_ = mx::detail::Notation::Infix;
    std::formatter<std::string_view, char> text_;
};

/// `std::format("{}", kind)` is mx::kindName(kind), with the usual string
/// options: `{:>8}`.
template <>
struct std::formatter<mx::Kind, char> : std::formatter<std::string_view, char> {
    auto format(mx::Kind kind, std::format_context &context) const {
        return std::formatter<std::string_view, char>::format(mx::kindName(kind), context);
    }
};
