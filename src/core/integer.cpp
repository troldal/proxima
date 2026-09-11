#include <mx/errors.hpp>
#include <mx/integer.hpp>

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

namespace mx {
namespace {

constexpr std::int64_t kMinInt64 = std::numeric_limits<std::int64_t>::min();
constexpr std::int64_t kMaxInt64 = std::numeric_limits<std::int64_t>::max();

} // namespace

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
    // prefixes among it -- so the digits are checked here first. A leading zero
    // is fine; it is not an octal prefix to this class.
    if (!std::all_of(text.begin(), text.end(),
                     [](char c) { return c >= '0' && c <= '9'; })) {
        return std::nullopt;
    }

    Backend value;
    value.assign(std::string(text));
    return Integer(negative ? Backend(-value) : std::move(value));
}

Integer::Integer(std::string_view text) {
    if (auto parsed = parse(text)) {
        *this = std::move(*parsed);
        return;
    }
    throw ParseError("not an integer: '" + std::string(text) + "'");
}

// --- inspection -----------------------------------------------------------

int Integer::sign() const { return value_.sign(); }

bool Integer::isSmall() const {
    return value_ >= kMinInt64 && value_ <= kMaxInt64;
}

std::optional<std::int64_t> Integer::toInt64() const {
    if (!isSmall()) {
        return std::nullopt;
    }
    return value_.convert_to<std::int64_t>();
}

std::string Integer::toString() const {
    // Worth the branch: std::to_string on an int64 is several times quicker
    // than cpp_int's general formatter, and most values printed are small.
    if (const auto small = toInt64()) {
        return std::to_string(*small);
    }
    return value_.str();
}

double Integer::toDouble() const { return value_.convert_to<double>(); }

std::size_t Integer::hash() const {
    // A value that fits in 64 bits can never equal one that does not, so the
    // two may be hashed by entirely separate routes -- which lets the common
    // case skip cpp_int's limb-walking hash. Every expression node is hashed
    // once when it is built, so this is not a rare path.
    if (const auto small = toInt64()) {
        return std::hash<std::int64_t>{}(*small);
    }
    return std::hash<Backend>{}(value_);
}

// --- arithmetic -----------------------------------------------------------

Integer Integer::operator-() const { return Integer(Backend(-value_)); }

Integer Integer::operator+(const Integer &other) const {
    return Integer(Backend(value_ + other.value_));
}

Integer Integer::operator-(const Integer &other) const {
    return Integer(Backend(value_ - other.value_));
}

Integer Integer::operator*(const Integer &other) const {
    return Integer(Backend(value_ * other.value_));
}

Integer Integer::operator/(const Integer &other) const {
    // cpp_int throws std::overflow_error here. Checking first keeps the failure
    // this library's own, and keeps the message useful.
    if (other.value_.is_zero()) {
        throw Error("division by zero");
    }
    return Integer(Backend(value_ / other.value_));
}

Integer Integer::operator%(const Integer &other) const {
    if (other.value_.is_zero()) {
        throw Error("division by zero");
    }
    return Integer(Backend(value_ % other.value_));
}

// --- comparison -----------------------------------------------------------

bool Integer::operator==(const Integer &other) const {
    return value_ == other.value_;
}

std::strong_ordering Integer::operator<=>(const Integer &other) const {
    if (value_ < other.value_) {
        return std::strong_ordering::less;
    }
    if (value_ > other.value_) {
        return std::strong_ordering::greater;
    }
    return std::strong_ordering::equal;
}

// --- free functions -------------------------------------------------------

Integer abs(const Integer &value) {
    return Integer(Integer::Backend(boost::multiprecision::abs(value.value_)));
}

Integer gcd(const Integer &a, const Integer &b) {
    // Boost's gcd is already non-negative and already gives gcd(0, 0) == 0,
    // which are this function's two documented edge cases.
    return Integer(
        Integer::Backend(boost::multiprecision::gcd(a.value_, b.value_)));
}

} // namespace mx
