#include <proxima/errors.hpp>
#include <proxima/integer.hpp>

#include "core/big_int.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <numeric>
#include <string>
#include <utility>

namespace proxima {
namespace {

using detail::BigInt;
using detail::IntegerAccess;
using detail::Wide;

constexpr std::int64_t kMinInt64 = std::numeric_limits<std::int64_t>::min();
constexpr std::int64_t kMaxInt64 = std::numeric_limits<std::int64_t>::max();

// Machine arithmetic that says when it overflowed rather than wrapping, which
// in signed arithmetic would be undefined. GCC and Clang (clang-cl included)
// have a builtin for each; MSVC takes the portable checks.

bool add_overflows(std::int64_t a, std::int64_t b, std::int64_t &out) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_add_overflow(a, b, &out);
#else
    if ((b > 0 && a > kMaxInt64 - b) || (b < 0 && a < kMinInt64 - b)) {
        return true;
    }
    out = a + b;
    return false;
#endif
}

bool sub_overflows(std::int64_t a, std::int64_t b, std::int64_t &out) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_sub_overflow(a, b, &out);
#else
    if ((b < 0 && a > kMaxInt64 + b) || (b > 0 && a < kMinInt64 + b)) {
        return true;
    }
    out = a - b;
    return false;
#endif
}

bool mul_overflows(std::int64_t a, std::int64_t b, std::int64_t &out) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_mul_overflow(a, b, &out);
#else
    if (a > 0) {
        if (b > 0 ? a > kMaxInt64 / b : b < kMinInt64 / a) {
            return true;
        }
    } else if (b > 0 ? a < kMinInt64 / b : (a != 0 && b < kMaxInt64 / a)) {
        return true;
    }
    out = a * b;
    return false;
#endif
}

/// |value| as an unsigned 64-bit number, which holds it even for the most
/// negative value, whose magnitude has no signed 64-bit representation.
std::uint64_t magnitude(std::int64_t value) {
    return value < 0 ? std::uint64_t{0} - static_cast<std::uint64_t>(value)
                     : static_cast<std::uint64_t>(value);
}

} // namespace

// --- the representation ---------------------------------------------------

Integer detail::IntegerAccess::from_wide(Wide value) {
    Integer result;
    if (value >= kMinInt64 && value <= kMaxInt64) {
        result.small_ = value.convert_to<std::int64_t>();
    } else {
        result.big_ = std::make_shared<const BigInt>(BigInt{std::move(value)});
    }
    return result;
}

Integer Integer::from_unsigned(std::uint64_t value) {
    return IntegerAccess::from_wide(Wide(value));
}

int Integer::big_sign() const { return big_->value.sign(); }

std::size_t Integer::big_hash() const { return std::hash<Wide>{}(big_->value); }

bool Integer::big_equal(const Integer &other) const {
    return big_->value == other.big_->value;
}

std::strong_ordering Integer::big_compare(const Integer &other) const {
    return IntegerAccess::with_wide(*this, other, [](const Wide &a, const Wide &b) {
        if (a < b) {
            return std::strong_ordering::less;
        }
        if (a > b) {
            return std::strong_ordering::greater;
        }
        return std::strong_ordering::equal;
    });
}

// --- construction ---------------------------------------------------------

std::optional<Integer> Integer::parse(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    bool negative = false;
    if (text.front() == '+' || text.front() == '-') {
        negative = text.front() == '-';
        text.remove_prefix(1);
    }
    if (text.empty()) {
        return std::nullopt;
    }

    // Almost every literal is short. Eighteen digits always fit in 64 bits, so
    // this needs no overflow check, and it is quicker than handing the text to
    // cpp_int, which must scan for a base prefix and then work in limbs.
    if (text.size() <= 18) {
        std::int64_t value = 0;
        for (const char c : text) {
            if (c < '0' || c > '9') {
                return std::nullopt;
            }
            value = value * 10 + (c - '0');
        }
        return Integer(negative ? -value : value);
    }

    // cpp_int's reader accepts rather more than a decimal literal -- 0x and 0b
    // prefixes among it -- so the digits are checked here first.
    if (!std::all_of(text.begin(), text.end(),
                     [](char c) { return c >= '0' && c <= '9'; })) {
        return std::nullopt;
    }

    // And a leading zero is an octal prefix to it. Found by fuzzing: a literal
    // of more than eighteen digits starting with 0 was read in base 8, so
    // "0052...8..." threw an exception that escaped the reader of Maxima's
    // replies, and one with no 8 or 9 in it silently had the wrong value.
    const std::size_t first_significant = text.find_first_not_of('0');
    if (first_significant == std::string_view::npos) {
        return Integer(0);
    }
    text.remove_prefix(first_significant);
    if (text.size() <= 18) {
        return parse(negative ? "-" + std::string(text) : std::string(text));
    }

    Wide value;
    value.assign(std::string(text));
    if (negative) {
        value = -value;
    }
    return IntegerAccess::from_wide(std::move(value));
}

Integer::Integer(std::string_view text) {
    if (auto parsed = parse(text)) {
        *this = std::move(*parsed);
        return;
    }
    throw ParseError("not an integer: '" + std::string(text) + "'");
}

// --- conversion -----------------------------------------------------------

std::string Integer::to_string() const {
    return big_ ? big_->value.str() : std::to_string(small_);
}

double Integer::to_double() const {
    return big_ ? big_->value.convert_to<double>() : static_cast<double>(small_);
}

// --- arithmetic -----------------------------------------------------------
//
// Each operation tries the machine first and falls back to cpp_int only when
// an operand is already large or the machine result would not fit.

Integer Integer::operator-() const {
    if (!big_ && small_ != kMinInt64) {
        return Integer(-small_);
    }
    return IntegerAccess::with_wide(*this, [](const Wide &a) {
        return IntegerAccess::from_wide(-a);
    });
}

Integer Integer::operator+(const Integer &other) const {
    std::int64_t sum = 0;
    if (!big_ && !other.big_ && !add_overflows(small_, other.small_, sum)) {
        return Integer(sum);
    }
    return IntegerAccess::with_wide(*this, other, [](const Wide &a, const Wide &b) {
        return IntegerAccess::from_wide(a + b);
    });
}

Integer Integer::operator-(const Integer &other) const {
    std::int64_t difference = 0;
    if (!big_ && !other.big_ && !sub_overflows(small_, other.small_, difference)) {
        return Integer(difference);
    }
    return IntegerAccess::with_wide(*this, other, [](const Wide &a, const Wide &b) {
        return IntegerAccess::from_wide(a - b);
    });
}

Integer Integer::operator*(const Integer &other) const {
    std::int64_t product = 0;
    if (!big_ && !other.big_ && !mul_overflows(small_, other.small_, product)) {
        return Integer(product);
    }
    return IntegerAccess::with_wide(*this, other, [](const Wide &a, const Wide &b) {
        return IntegerAccess::from_wide(a * b);
    });
}

Integer Integer::operator/(const Integer &other) const {
    // cpp_int throws std::overflow_error here. Checking first keeps the failure
    // this library's own, and keeps the message useful.
    if (other.is_zero()) {
        throw Error("division by zero");
    }
    // The one small quotient that does not fit: the most negative value over -1.
    if (!big_ && !other.big_ && !(small_ == kMinInt64 && other.small_ == -1)) {
        return Integer(small_ / other.small_);
    }
    return IntegerAccess::with_wide(*this, other, [](const Wide &a, const Wide &b) {
        return IntegerAccess::from_wide(a / b);
    });
}

Integer Integer::operator%(const Integer &other) const {
    if (other.is_zero()) {
        throw Error("division by zero");
    }
    if (!big_ && !other.big_) {
        // x % -1 is 0, but computing it for the most negative x overflows.
        return Integer(other.small_ == -1 ? 0 : small_ % other.small_);
    }
    return IntegerAccess::with_wide(*this, other, [](const Wide &a, const Wide &b) {
        return IntegerAccess::from_wide(a % b);
    });
}

// --- free functions -------------------------------------------------------

Integer detail::abs_of(const Integer &value) {
    if (IntegerAccess::is_small(value)) {
        // Through the unsigned magnitude, so the most negative value becomes
        // the large 2^63 rather than overflowing.
        return Integer(magnitude(IntegerAccess::small(value)));
    }
    return IntegerAccess::with_wide(value, [](const Wide &a) {
        return IntegerAccess::from_wide(boost::multiprecision::abs(a));
    });
}

Integer detail::gcd_of(const Integer &a, const Integer &b) {
    // Non-negative, and gcd(0, 0) == 0: this function's two documented edge
    // cases, which std::gcd on magnitudes and Boost's gcd both give.
    if (IntegerAccess::is_small(a) && IntegerAccess::is_small(b)) {
        return Integer(std::gcd(magnitude(IntegerAccess::small(a)),
                                magnitude(IntegerAccess::small(b))));
    }
    return IntegerAccess::with_wide(a, b, [](const Wide &x, const Wide &y) {
        return IntegerAccess::from_wide(boost::multiprecision::gcd(x, y));
    });
}

} // namespace proxima
