#pragma once

#include <boost/multiprecision/cpp_int.hpp>

#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>

namespace mx {

/// The character types. Integral to C++, but not numbers to anyone reading an
/// expression: `'a'` in a formula is a mistake, not the value 97.
template <typename T>
concept CharacterType = std::same_as<T, char> || std::same_as<T, wchar_t>
                        || std::same_as<T, char8_t> || std::same_as<T, char16_t>
                        || std::same_as<T, char32_t>;

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
// The implementations behind mx::abs and mx::gcd, which are templates only to
// constrain what they accept.
Integer absOf(const Integer &value);
Integer gcdOf(const Integer &a, const Integer &b);
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
    Integer(T value) : value_(static_cast<std::int64_t>(value)) {} // NOLINT

    /// Unsigned values above the signed range still fit, so they are not
    /// quietly truncated into negatives.
    template <typename T>
        requires IntegralNumber<T> && std::unsigned_integral<T>
    Integer(T value) // NOLINT
        : value_(static_cast<std::uint64_t>(value)) {}

    /// Deleted. This used to refuse bool and char but accept the wide
    /// character types, so `Integer(u'7')` compiled and meant 55.
    template <typename T>
        requires BooleanOrCharacter<T>
    Integer(T) = delete;

    /// Reads a decimal literal, with an optional sign. Throws mx::ParseError if
    /// `text` is not one.
    explicit Integer(std::string_view text);

    /// Reads a decimal literal, or nothing if `text` is not one.
    static std::optional<Integer> parse(std::string_view text);

    /// -1, 0 or 1.
    int sign() const;
    bool isZero() const { return value_.is_zero(); }
    bool isNegative() const { return sign() < 0; }

    /// The value, when it fits in 64 bits.
    std::optional<std::int64_t> toInt64() const;

    /// True when the value fits in 64 bits, which is the common case.
    ///
    /// cpp_int holds a value that small within the object, so this also means
    /// no heap storage is in use — but the guarantee this makes, and the one
    /// the fast paths below rest on, is about the range and not the allocation.
    bool isSmall() const;

    std::string toString() const;
    double toDouble() const;
    std::size_t hash() const;

    Integer operator-() const;
    Integer operator+(const Integer &other) const;
    Integer operator-(const Integer &other) const;
    Integer operator*(const Integer &other) const;

    /// Truncating division, as C++ spells it: the quotient rounds toward zero
    /// and the remainder takes the dividend's sign. Throws mx::Error on a zero
    /// divisor.
    Integer operator/(const Integer &other) const;
    Integer operator%(const Integer &other) const;

    Integer &operator+=(const Integer &other) { return *this = *this + other; }
    Integer &operator-=(const Integer &other) { return *this = *this - other; }
    Integer &operator*=(const Integer &other) { return *this = *this * other; }
    Integer &operator/=(const Integer &other) { return *this = *this / other; }

    bool operator==(const Integer &other) const;
    std::strong_ordering operator<=>(const Integer &other) const;

private:
    friend Integer detail::absOf(const Integer &value);
    friend Integer detail::gcdOf(const Integer &a, const Integer &b);

    using Backend = boost::multiprecision::cpp_int;

    explicit Integer(Backend value) : value_(std::move(value)) {}

    Backend value_ = 0;
};

/// The absolute value. Takes an Integer and nothing else: a plain
/// `const Integer &` would make this a candidate for abs(-3) through the
/// implicit constructor, which is <cstdlib>'s call to answer. See
/// mx::ExprArgument for why that matters.
template <std::same_as<Integer> T>
Integer abs(const T &value) {
    return detail::absOf(value);
}

namespace detail {
inline const Integer &asInteger(const Integer &value) {
    return value;
}
template <IntegralNumber T>
Integer asInteger(T value) {
    return Integer(value);
}
} // namespace detail

/// Greatest common divisor, non-negative. gcd(0, 0) is 0.
///
/// At least one argument must be an Integer, and the other may be an integral
/// number — `gcd(n, 1001)` — so that gcd(12, 18) stays std::gcd's, for the
/// reason abs takes only an Integer.
template <typename A, typename B>
    requires(std::same_as<A, Integer> && (std::same_as<B, Integer> || IntegralNumber<B>))
            || (IntegralNumber<A> && std::same_as<B, Integer>)
Integer gcd(const A &a, const B &b) {
    return detail::gcdOf(detail::asInteger(a), detail::asInteger(b));
}

/// Writes the decimal digits, as toString() does.
std::ostream &operator<<(std::ostream &out, const Integer &value);

} // namespace mx

template <>
struct std::hash<mx::Integer> {
    std::size_t operator()(const mx::Integer &value) const noexcept {
        return value.hash();
    }
};

/// `std::format("{}", value)` is the decimal digits, with the usual string
/// options for width and alignment: `{:>40}`.
template <>
struct std::formatter<mx::Integer, char> : std::formatter<std::string_view, char> {
    auto format(const mx::Integer &value, std::format_context &context) const {
        const std::string digits = value.toString();
        return std::formatter<std::string_view, char>::format(std::string_view(digits),
                                                              context);
    }
};
