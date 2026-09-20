#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace proxima::detail {

/// A generic s-expression: the shape Maxima's internal representation arrives
/// in, with none of its meaning attached.
///
/// Deliberately knows nothing about Maxima. `MPLUS`, `$X` and `SIMP` are just
/// symbols here; interpreting them is the mapping layer's job (docs/design.md step
/// 9). Keeping the two apart is what lets each be tested on its own — this one
/// entirely from string literals.
///
/// ## The variant is the kind
///
/// One alternative per Kind, and `kind()` is read off which one is held, as
/// `detail::Node` does. It used to be a kind tag beside `text`, `real` and
/// `items`, all four present whatever the kind, so `digits()` on a symbol
/// returned the symbol's name — "meaningless for other kinds", said the
/// comment that had to be obeyed rather than the type.
///
/// ## Why integers are text
///
/// Maxima produces bignums in ordinary use: `30!` is
/// `265252859812191058636308480000000`, well past `int64`. Storing the digits
/// keeps the reader lossless and leaves the choice of numeric representation to
/// the layer that has to make it. Use as_int64() when the value fits and the
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

    // Three of the five kinds carry a string. Each has a type of its own, so
    // what tells the digits of an integer from the name of a symbol is which
    // alternative holds them rather than a tag beside them.

    /// Kind::Integer. A sign, optionally, then at least one digit — which the
    /// reader guarantees.
    struct Digits {
        std::string text;
        bool operator==(const Digits &) const = default;
    };

    /// Kind::Symbol.
    struct Name {
        std::string text;
        bool operator==(const Name &) const = default;
    };

    /// Kind::String, already unescaped.
    struct Text {
        std::string text;
        bool operator==(const Text &) const = default;
    };

    /// One alternative per Kind, in Kind's order, so that kind() is the
    /// variant's index.
    using Payload = std::variant<Digits, double, Name, Text, std::vector<SExpr>>;

    /// The empty list, which is what the reader starts each element as.
    SExpr() : payload_(std::vector<SExpr>{}) {}

    static SExpr integer(std::string digits);
    static SExpr real(double value);
    static SExpr symbol(std::string name);
    static SExpr string(std::string text);
    static SExpr list(std::vector<SExpr> items);

    Kind kind() const {
        // One case per alternative, in the variant's order. A missing case is
        // a compile error at the static_assert below, not a wrong answer.
        switch (payload_.index()) {
        case 0:
            return Kind::Integer;
        case 1:
            return Kind::Real;
        case 2:
            return Kind::Symbol;
        case 3:
            return Kind::String;
        default:
            break;
        }
        return Kind::List;
    }

    bool is_integer() const { return kind() == Kind::Integer; }
    bool is_real() const { return kind() == Kind::Real; }
    bool is_symbol() const { return kind() == Kind::Symbol; }
    bool is_string() const { return kind() == Kind::String; }
    bool is_list() const { return kind() == Kind::List; }

    /// True when this is the symbol `name`. The common test in the mapping
    /// layer, where most questions are "is this head MPLUS?".
    bool is_symbol(std::string_view name) const {
        const auto *symbol = std::get_if<Name>(&payload_);
        return symbol != nullptr && symbol->text == name;
    }

    // Each of these is for a kind already established — is_integer() and the
    // rest, or a switch on kind(). Asking the wrong one throws
    // std::bad_variant_access, where it used to return another kind's field.

    /// The digits of an Integer, sign included.
    const std::string &digits() const { return std::get<Digits>(payload_).text; }

    /// The Integer's value, or nullopt when it does not fit in 64 bits.
    std::optional<std::int64_t> as_int64() const;

    double real_value() const { return std::get<double>(payload_); }
    const std::string &symbol_name() const { return std::get<Name>(payload_).text; }
    const std::string &string_value() const { return std::get<Text>(payload_).text; }

    /// Number of elements in a List; 0 for every other kind, as a leaf has no
    /// children. This one is answerable for all of them, so it answers.
    std::size_t size() const {
        const auto *items = std::get_if<std::vector<SExpr>>(&payload_);
        return items != nullptr ? items->size() : 0;
    }
    bool empty() const { return size() == 0; }

    /// Element `index` of a List. Throws ParseError if out of range, since
    /// reaching past the end means the reply was not the shape expected.
    const SExpr &at(std::size_t index) const;

    /// Renders back to s-expression text. Round-trips through parse_sexpr for
    /// everything the reader accepts; used for diagnostics and test failures.
    std::string to_string() const;

    /// Equal payloads: different kinds are never equal, and the same kind
    /// compares whatever that kind holds.
    bool operator==(const SExpr &other) const = default;

private:
    explicit SExpr(Payload held) : payload_(std::move(held)) {}

    Payload payload_;
};

static_assert(std::variant_size_v<SExpr::Payload> == 5,
              "SExpr::kind() maps each alternative to a Kind by index; a new "
              "alternative needs a Kind and a case there");

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
SExpr parse_sexpr(std::string_view text);

/// Nesting limit, chosen so that a pathological reply cannot overflow the stack
/// when the resulting tree is destroyed. Far beyond any real expression.
inline constexpr std::size_t kMaxSExprDepth = 1000;

} // namespace proxima::detail
