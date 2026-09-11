#pragma once

#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mx {

/// An exact integer of unbounded size.
///
/// Maxima produces large integers in ordinary use — `30!` has 33 digits, and a
/// factored polynomial's coefficients grow quickly — so a fixed width would
/// mean either wrapping (wrong) or refusing (useless). Values that fit in 64
/// bits are held inline and never allocate, which is almost all of them; only
/// the rest reach for storage.
///
/// Written here rather than taken from a multiprecision library because this
/// library has no third-party dependencies, and that is worth more than the few
/// hundred lines below. The arithmetic is schoolbook: adequate for the sizes
/// Maxima hands back, and checked against Maxima itself in the tests, which is
/// as good an oracle as exists.
class Integer {
public:
    Integer() = default;

    /// Implicit from any integral type but bool and char.
    ///
    /// A template rather than overloads on int and std::int64_t: those leave
    /// `long long` ambiguous wherever int64_t is `long`, which is every LP64
    /// platform — it compiles on Windows and not on Linux.
    template <typename T>
        requires std::signed_integral<T> && (!std::same_as<T, char>)
    Integer(T value) : small_(static_cast<std::int64_t>(value)) {} // NOLINT

    /// Unsigned values above the signed range still fit, so they are not
    /// quietly truncated into negatives.
    template <typename T>
        requires std::unsigned_integral<T> && (!std::same_as<T, bool>)
                 && (!std::same_as<T, char>)
    Integer(T value) { // NOLINT
        assignUnsigned(static_cast<std::uint64_t>(value));
    }

    /// Reads a decimal literal, with an optional sign. Throws mx::ParseError if
    /// `text` is not one.
    explicit Integer(std::string_view text);

    /// Reads a decimal literal, or nothing if `text` is not one.
    static std::optional<Integer> parse(std::string_view text);

    /// -1, 0 or 1.
    int sign() const;
    bool isZero() const { return sign() == 0; }
    bool isNegative() const { return sign() < 0; }

    /// The value, when it fits in 64 bits.
    std::optional<std::int64_t> toInt64() const;

    /// True when no storage is in use, which is the common case.
    bool isSmall() const { return limbs_.empty(); }

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
    friend Integer abs(const Integer &value);
    friend Integer gcd(const Integer &a, const Integer &b);

    void assignUnsigned(std::uint64_t value);

    /// Ensures the value is held in `limbs_`, so the general routines apply.
    void promote();
    /// Returns to the inline form when the value fits again.
    void demote();

    // Class invariant, on which several fast paths depend: `limbs_` is empty
    // *exactly when* the value fits in 64 bits. Every operation calls demote()
    // before returning, so a value that shrinks back into range returns to the
    // inline form. Two consequences worth naming, since correctness rests on
    // them: an inline value and a stored one can never be equal, and so they
    // may be hashed and compared by entirely separate routes.

    /// The whole value, while `limbs_` is empty.
    std::int64_t small_ = 0;
    /// Magnitude, least significant limb first, with no leading zero limb.
    std::vector<std::uint32_t> limbs_;
    /// Sign of the magnitude; meaningless while `limbs_` is empty.
    bool negative_ = false;
};

Integer abs(const Integer &value);

/// Greatest common divisor, non-negative. gcd(0, 0) is 0.
Integer gcd(const Integer &a, const Integer &b);

} // namespace mx

template <>
struct std::hash<mx::Integer> {
    std::size_t operator()(const mx::Integer &value) const noexcept {
        return value.hash();
    }
};
