#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mx::detail {

/// A generic s-expression: the shape Maxima's internal representation arrives
/// in, with none of its meaning attached.
///
/// Deliberately knows nothing about Maxima. `MPLUS`, `$X` and `SIMP` are just
/// symbols here; interpreting them is the mapping layer's job (PLAN.md step 9).
/// Keeping the two apart is what lets each be tested on its own — this one
/// entirely from string literals.
///
/// ## Why integers are text
///
/// Maxima produces bignums in ordinary use: `30!` is
/// `265252859812191058636308480000000`, well past `int64`. Storing the digits
/// keeps the reader lossless and leaves the choice of numeric representation to
/// the layer that has to make it. Use asInt64() when the value fits and the
/// digits otherwise.
class SExpr {
public:
    enum class Kind {
        Integer, ///< Arbitrary-precision integer, held as digits.
        Real,    ///< Double-precision float.
        Symbol,  ///< `MPLUS`, `$X`, `%SIN`, `NIL`, `T`.
        String,  ///< A Lisp string, unescaped.
        List,    ///< `(...)`, possibly empty.
    };

    SExpr() : kind_(Kind::List) {}

    static SExpr integer(std::string digits);
    static SExpr real(double value);
    static SExpr symbol(std::string name);
    static SExpr string(std::string text);
    static SExpr list(std::vector<SExpr> items);

    Kind kind() const { return kind_; }
    bool isInteger() const { return kind_ == Kind::Integer; }
    bool isReal() const { return kind_ == Kind::Real; }
    bool isSymbol() const { return kind_ == Kind::Symbol; }
    bool isString() const { return kind_ == Kind::String; }
    bool isList() const { return kind_ == Kind::List; }

    /// True when this is the symbol `name`. The common test in the mapping
    /// layer, where most questions are "is this head MPLUS?".
    bool isSymbol(std::string_view name) const {
        return kind_ == Kind::Symbol && text_ == name;
    }

    /// The digits of an Integer, sign included: an optional sign and at least
    /// one digit, which the reader guarantees. Meaningless for other kinds,
    /// whose text it would return — check isInteger() first.
    const std::string &digits() const { return text_; }

    /// The Integer's value, or nullopt when it does not fit in 64 bits.
    std::optional<std::int64_t> asInt64() const;

    double realValue() const { return real_; }
    const std::string &symbolName() const { return text_; }
    const std::string &stringValue() const { return text_; }

    const std::vector<SExpr> &items() const { return items_; }

    /// Number of elements in a List; 0 for every other kind.
    std::size_t size() const { return items_.size(); }
    bool empty() const { return items_.empty(); }

    /// Element `index` of a List. Throws ParseError if out of range, since
    /// reaching past the end means the reply was not the shape expected.
    const SExpr &at(std::size_t index) const;

    /// Renders back to s-expression text. Round-trips through parseSExpr for
    /// everything the reader accepts; used for diagnostics and test failures.
    std::string toString() const;

    bool operator==(const SExpr &other) const;

private:
    Kind kind_;
    std::string text_;
    double real_ = 0.0;
    std::vector<SExpr> items_;
};

/// Reads exactly one s-expression from `text`.
///
/// Throws ParseError on anything malformed: unbalanced parentheses, more than
/// one expression, an unterminated string, or nesting past the depth limit.
///
/// Accepts what SBCL's printer emits for Maxima terms: integers (any size),
/// floats with any of the Lisp exponent markers, symbols including the
/// `|bar quoted|` form, strings with backslash escapes, and lists. Dotted pairs
/// are rejected explicitly rather than silently mis-read — Maxima's term
/// representation uses proper lists throughout, so one appearing would mean
/// something unmodelled had arrived.
SExpr parseSExpr(std::string_view text);

/// Nesting limit, chosen so that a pathological reply cannot overflow the stack
/// when the resulting tree is destroyed. Far beyond any real expression.
inline constexpr std::size_t kMaxSExprDepth = 1000;

} // namespace mx::detail
