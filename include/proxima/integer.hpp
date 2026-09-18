#pragma once

#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <iosfwd>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace proxima {

/// The character types. Integral to C++, but not numbers to anyone reading an
/// expression: `'a'` in a formula is a mistake, not the value 97.
template <typename T>
concept CharacterType
    = std::same_as<T, char> || std::same_as<T, wchar_t> || std::same_as<T, char8_t>
      || std::same_as<T, char16_t> || std::same_as<T, char32_t>;

/// The integral types that are numbers: all of them except bool and the
/// character types.
///
/// `signed char` and `unsigned char` count as numbers, because std::int8_t and
/// std::uint8_t are spelled with them, and excluding them would make those
/// unusable. Plain `char`, whose signedness is not even fixed, does not.
template <typename T>
concept IntegralNumber
    = std::integral<T> && !std::same_as<T, bool> && !CharacterType<T>;

/// The types C++ converts to numbers implicitly that must not become numbers
/// here. Integer and Expr delete their constructors for these, so that a
/// mistake like `Expr(true)` is a compile error naming the argument rather
/// than a silent 1.
template <typename T>
concept BooleanOrCharacter = std::same_as<T, bool> || CharacterType<T>;

class Integer;

namespace detail {
// The implementations behind proxima::abs and proxima::gcd, which are templates only
// to constrain what they accept.
Integer abs_of(const Integer &value);
Integer gcd_of(const Integer &a, const Integer &b);

// A value too large for 64 bits. Defined, with the arithmetic backend, only in
// the library's own sources; see Integer's layout note.
struct BigInt;
// The library's own way in to that representation.
struct IntegerAccess;
} // namespace detail

/// An exact integer of unbounded size.
///
/// Maxima produces large integers in ordinary use — `30!` has 33 digits, and a
/// factored polynomial's coefficients grow quickly — so a fixed width would
/// mean either wrapping (wrong) or refusing (useless).
///
/// The arithmetic is Boost.Multiprecision's `cpp_int`. An earlier version of
/// this class carried its own schoolbook implementation, on the grounds that
/// the library had no third-party dependencies. That turned out to be a poor
/// trade twice over. Its division was binary long division — O(bits × limbs)
/// however small the divisor — which made `30!/7` some forty-five times slower
/// than cpp_int, on a hot path, since reducing a rational divides by the gcd
/// after every fold. And several hundred lines of hand-written carry, borrow
/// and division logic fail by producing wrong answers rather than by crashing,
/// which is the worst failure a computer algebra system can have. cpp_int has
/// had two decades of other people finding those.
///
/// This remains a facade rather than an alias: it fixes the spelling of the
/// operations, keeps the fast paths below, and leaves room to change backend
/// again without touching a line of consumer code.
///
/// ## Layout, and why Boost is not in this header
///
/// A value that fits in 64 bits — nearly every value — is held inline, and
/// arithmetic on two such values is plain machine arithmetic with an overflow
/// check. Only a value that does not fit is handed to cpp_int, held behind a
/// pointer to an immutable object. So this header needs nothing of Boost: it
/// used to hold a cpp_int by value, which put Boost.Multiprecision into every
/// translation unit that included <proxima/expr.hpp> — measured, 118,000
/// preprocessed lines and 0.85 s of compile time each — and made Boost's
/// headers part of the installed package.
///
/// The representation is canonical: the pointer is set exactly when the value
/// does not fit in 64 bits. So two small values compare by their inline field
/// alone, and a small value never equals a large one.
class Integer {
public:
    Integer() = default;

    /// Implicit from any integral type that is a number: see IntegralNumber.
    ///
    /// A template rather than overloads on int and std::int64_t: those leave
    /// `long long` ambiguous wherever int64_t is `long`, which is every LP64
    /// platform — it compiles on Windows and not on Linux.
    template <typename T>
        requires IntegralNumber<T> && std::signed_integral<T>
    Integer(T value) : small_(static_cast<std::int64_t>(value)) {} // NOLINT

    /// Unsigned values above the signed range still fit, so they are not
    /// quietly truncated into negatives.
    template <typename T>
        requires IntegralNumber<T> && std::unsigned_integral<T>
    Integer(T value) { // NOLINT
        const auto wide = static_cast<std::uint64_t>(value);
        if (wide <= static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())) {
            small_ = static_cast<std::int64_t>(wide);
        } else {
            *this = from_unsigned(wide);
        }
    }

    /// Deleted. This used to refuse bool and char but accept the wide
    /// character types, so `Integer(u'7')` compiled and meant 55.
    template <typename T>
        requires BooleanOrCharacter<T>
    Integer(T) = delete;

    /// Reads a decimal literal, with an optional sign. Throws proxima::ParseError if
    /// `text` is not one.
    explicit Integer(std::string_view text);

    /// Reads a decimal literal, or nothing if `text` is not one.
    static std::optional<Integer> parse(std::string_view text);

    /// -1, 0 or 1.
    int sign() const { return big_ ? big_sign() : (small_ > 0) - (small_ < 0); }
    bool is_zero() const { return !big_ && small_ == 0; }
    bool is_negative() const { return sign() < 0; }

    /// The value, when it fits in 64 bits.
    std::optional<std::int64_t> to_int64() const {
        if (big_) {
            return std::nullopt;
        }
        return small_;
    }

    /// True when the value fits in 64 bits, which is the common case, and then
    /// it is held inline, with no heap storage.
    bool is_small() const { return !big_; }

    std::string to_string() const;
    double to_double() const;
    std::size_t hash() const {
        // A value that fits in 64 bits can never equal one that does not, so
        // the two may be hashed by entirely separate routes. Every expression
        // node is hashed once when it is built, so this is not a rare path.
        return big_ ? big_hash() : std::hash<std::int64_t>{}(small_);
    }

    Integer operator-() const;
    Integer operator+(const Integer &other) const;
    Integer operator-(const Integer &other) const;
    Integer operator*(const Integer &other) const;

    /// Truncating division, as C++ spells it: the quotient rounds toward zero
    /// and the remainder takes the dividend's sign. Throws proxima::Error on a zero
    /// divisor.
    Integer operator/(const Integer &other) const;
    Integer operator%(const Integer &other) const;

    Integer &operator+=(const Integer &other) { return *this = *this + other; }
    Integer &operator-=(const Integer &other) { return *this = *this - other; }
    Integer &operator*=(const Integer &other) { return *this = *this * other; }
    Integer &operator/=(const Integer &other) { return *this = *this / other; }

    bool operator==(const Integer &other) const {
        // Canonical, so a small value never equals a large one.
        if (!big_ || !other.big_) {
            return !big_ && !other.big_ && small_ == other.small_;
        }
        return big_equal(other);
    }
    std::strong_ordering operator<=>(const Integer &other) const {
        if (!big_ && !other.big_) {
            return small_ <=> other.small_;
        }
        return big_compare(other);
    }

private:
    friend struct detail::IntegerAccess;

    static Integer from_unsigned(std::uint64_t value);
    int big_sign() const;
    std::size_t big_hash() const;
    bool big_equal(const Integer &other) const;
    std::strong_ordering big_compare(const Integer &other) const;

    /// The value, when big_ is null.
    std::int64_t small_ = 0;
    /// The value, when it does not fit in 64 bits; null otherwise. Immutable
    /// and shared, so copying a large Integer copies a pointer.
    std::shared_ptr<const detail::BigInt> big_;
};

/// The absolute value. Takes an Integer and nothing else: a plain
/// `const Integer &` would make this a candidate for abs(-3) through the
/// implicit constructor, which is <cstdlib>'s call to answer. See
/// proxima::ExprArgument for why that matters.
template <std::same_as<Integer> T>
Integer abs(const T &value) {
    return detail::abs_of(value);
}

namespace detail {
// Returns its own parameter by reference: only ever called on gcd's arguments,
// which outlive the call the reference is used in.
inline const Integer &as_integer(const Integer &value) {
    return value; // NOLINT(bugprone-return-const-ref-from-parameter)
}
template <IntegralNumber T>
Integer as_integer(T value) {
    return Integer(value);
}
} // namespace detail

/// Greatest common divisor, non-negative. gcd(0, 0) is 0.
///
/// At least one argument must be an Integer, and the other may be an integral
/// number — `gcd(n, 1001)` — so that gcd(12, 18) stays std::gcd's, for the
/// reason abs takes only an Integer.
template <typename A, typename B>
    requires(std::same_as<A, Integer>
             && (std::same_as<B, Integer> || IntegralNumber<B>))
            || (IntegralNumber<A> && std::same_as<B, Integer>)
Integer gcd(const A &a, const B &b) {
    return detail::gcd_of(detail::as_integer(a), detail::as_integer(b));
}

/// Writes the decimal digits, as to_string() does.
std::ostream &operator<<(std::ostream &out, const Integer &value);

} // namespace proxima

template <>
struct std::hash<proxima::Integer> {
    std::size_t operator()(const proxima::Integer &value) const noexcept {
        return value.hash();
    }
};

/// `std::format("{}", value)` is the decimal digits, with the usual string
/// options for width and alignment: `{:>40}`.
template <>
struct std::formatter<proxima::Integer, char>
    : std::formatter<std::string_view, char> {
    auto format(const proxima::Integer &value, std::format_context &context) const {
        const std::string digits = value.to_string();
        return std::formatter<std::string_view, char>::format(
            std::string_view(digits), context);
    }
};
