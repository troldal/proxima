#include <mx/errors.hpp>
#include <mx/integer.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace mx {
namespace {

using Limbs = std::vector<std::uint32_t>;
constexpr std::uint64_t kBase = 1ULL << 32;

// --- the fast path --------------------------------------------------------
//
// Checked 64-bit arithmetic. When it fits, nothing here allocates; only on
// overflow does the general machinery below come into play.

constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();

bool addOverflows(std::int64_t a, std::int64_t b, std::int64_t &out) {
    if ((b > 0 && a > kMax - b) || (b < 0 && a < kMin - b)) {
        return true;
    }
    out = a + b;
    return false;
}

bool mulOverflows(std::int64_t a, std::int64_t b, std::int64_t &out) {
    if (a == 0 || b == 0) {
        out = 0;
        return false;
    }
    if (a == -1) {
        if (b == kMin) {
            return true;
        }
        out = -b;
        return false;
    }
    if (b == -1) {
        if (a == kMin) {
            return true;
        }
        out = -a;
        return false;
    }
    if (a > 0 ? (b > 0 ? a > kMax / b : b < kMin / a)
              : (b > 0 ? a < kMin / b : a < kMax / b)) {
        return true;
    }
    out = a * b;
    return false;
}

// --- magnitude arithmetic -------------------------------------------------

void trim(Limbs &limbs) {
    while (!limbs.empty() && limbs.back() == 0) {
        limbs.pop_back();
    }
}

int compareMagnitude(const Limbs &a, const Limbs &b) {
    if (a.size() != b.size()) {
        return a.size() < b.size() ? -1 : 1;
    }
    for (std::size_t i = a.size(); i-- > 0;) {
        if (a[i] != b[i]) {
            return a[i] < b[i] ? -1 : 1;
        }
    }
    return 0;
}

Limbs addMagnitude(const Limbs &a, const Limbs &b) {
    Limbs sum;
    sum.reserve(std::max(a.size(), b.size()) + 1);
    std::uint64_t carry = 0;
    for (std::size_t i = 0; i < std::max(a.size(), b.size()) || carry != 0; ++i) {
        std::uint64_t total = carry;
        if (i < a.size()) {
            total += a[i];
        }
        if (i < b.size()) {
            total += b[i];
        }
        sum.push_back(static_cast<std::uint32_t>(total & 0xffffffffULL));
        carry = total >> 32;
    }
    return sum;
}

/// `a - b`, requiring a >= b.
Limbs subtractMagnitude(const Limbs &a, const Limbs &b) {
    Limbs difference;
    difference.reserve(a.size());
    std::int64_t borrow = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        std::int64_t value = static_cast<std::int64_t>(a[i]) - borrow
                             - (i < b.size() ? static_cast<std::int64_t>(b[i]) : 0);
        borrow = value < 0 ? 1 : 0;
        if (value < 0) {
            value += static_cast<std::int64_t>(kBase);
        }
        difference.push_back(static_cast<std::uint32_t>(value));
    }
    trim(difference);
    return difference;
}

Limbs multiplyMagnitude(const Limbs &a, const Limbs &b) {
    if (a.empty() || b.empty()) {
        return {};
    }
    Limbs product(a.size() + b.size(), 0);
    for (std::size_t i = 0; i < a.size(); ++i) {
        std::uint64_t carry = 0;
        for (std::size_t j = 0; j < b.size() || carry != 0; ++j) {
            std::uint64_t total = product[i + j] + carry;
            if (j < b.size()) {
                total += static_cast<std::uint64_t>(a[i]) * b[j];
            }
            product[i + j] = static_cast<std::uint32_t>(total & 0xffffffffULL);
            carry = total >> 32;
        }
    }
    trim(product);
    return product;
}

void shiftLeftOne(Limbs &limbs) {
    std::uint32_t carry = 0;
    for (std::uint32_t &limb : limbs) {
        const std::uint32_t next = limb >> 31;
        limb = (limb << 1) | carry;
        carry = next;
    }
    if (carry != 0) {
        limbs.push_back(carry);
    }
}

void shiftRightOne(Limbs &limbs) {
    std::uint32_t carry = 0;
    for (std::size_t i = limbs.size(); i-- > 0;) {
        const std::uint32_t next = limbs[i] & 1u;
        limbs[i] = (limbs[i] >> 1) | (carry << 31);
        carry = next;
    }
    trim(limbs);
}

bool isEvenMagnitude(const Limbs &limbs) {
    return limbs.empty() || (limbs[0] & 1u) == 0;
}

std::size_t bitLength(const Limbs &limbs) {
    if (limbs.empty()) {
        return 0;
    }
    std::size_t bits = (limbs.size() - 1) * 32;
    std::uint32_t top = limbs.back();
    while (top != 0) {
        ++bits;
        top >>= 1;
    }
    return bits;
}

bool testBit(const Limbs &limbs, std::size_t bit) {
    const std::size_t limb = bit / 32;
    return limb < limbs.size() && ((limbs[limb] >> (bit % 32)) & 1u) != 0;
}

/// Binary long division: shift the dividend's bits into a remainder one at a
/// time, subtracting the divisor whenever it fits.
///
/// Knuth's algorithm D would be faster, and considerably easier to get subtly
/// wrong. The numbers here are small — a factorial, a polynomial coefficient —
/// so the simpler algorithm is the better trade.
void divideMagnitude(const Limbs &dividend, const Limbs &divisor, Limbs &quotient,
                     Limbs &remainder) {
    quotient.assign(dividend.size(), 0);
    remainder.clear();

    for (std::size_t bit = bitLength(dividend); bit-- > 0;) {
        shiftLeftOne(remainder);
        if (testBit(dividend, bit)) {
            if (remainder.empty()) {
                remainder.push_back(1);
            } else {
                remainder[0] |= 1u;
            }
        }
        if (compareMagnitude(remainder, divisor) >= 0) {
            remainder = subtractMagnitude(remainder, divisor);
            quotient[bit / 32] |= 1u << (bit % 32);
        }
    }
    trim(quotient);
    trim(remainder);
}

/// Divides by a value small enough for one limb, returning the remainder.
std::uint32_t divideBySmall(Limbs &limbs, std::uint32_t divisor) {
    std::uint64_t remainder = 0;
    for (std::size_t i = limbs.size(); i-- > 0;) {
        const std::uint64_t current = (remainder << 32) | limbs[i];
        limbs[i] = static_cast<std::uint32_t>(current / divisor);
        remainder = current % divisor;
    }
    trim(limbs);
    return static_cast<std::uint32_t>(remainder);
}

Limbs magnitudeOf(std::int64_t value) {
    // Built from the unsigned magnitude, so the extreme negative value needs no
    // special case — negating it would overflow.
    auto magnitude = static_cast<std::uint64_t>(value);
    if (value < 0) {
        magnitude = ~magnitude + 1;
    }
    Limbs limbs;
    while (magnitude != 0) {
        limbs.push_back(static_cast<std::uint32_t>(magnitude & 0xffffffffULL));
        magnitude >>= 32;
    }
    return limbs;
}

} // namespace

// --- construction ---------------------------------------------------------

void Integer::assignUnsigned(std::uint64_t value) {
    if (value <= static_cast<std::uint64_t>(kMax)) {
        small_ = static_cast<std::int64_t>(value);
        return;
    }
    // Past the signed range, so it needs storage rather than truncation.
    while (value != 0) {
        limbs_.push_back(static_cast<std::uint32_t>(value & 0xffffffffULL));
        value >>= 32;
    }
    negative_ = false;
}

void Integer::promote() {
    if (!limbs_.empty()) {
        return;
    }
    negative_ = small_ < 0;
    limbs_ = magnitudeOf(small_);
    small_ = 0;
}

void Integer::demote() {
    if (limbs_.empty()) {
        return;
    }
    if (limbs_.size() > 2) {
        return;
    }
    std::uint64_t magnitude = limbs_[0];
    if (limbs_.size() == 2) {
        magnitude |= static_cast<std::uint64_t>(limbs_[1]) << 32;
    }

    // The extreme negative value has no positive counterpart, so it is checked
    // against the unsigned magnitude rather than by negating.
    constexpr auto kMaxMagnitude = static_cast<std::uint64_t>(kMax);
    if (negative_ ? magnitude > kMaxMagnitude + 1 : magnitude > kMaxMagnitude) {
        return;
    }

    small_ = negative_ ? -static_cast<std::int64_t>(magnitude)
                       : static_cast<std::int64_t>(magnitude);
    if (negative_ && magnitude == kMaxMagnitude + 1) {
        small_ = kMin;
    }
    limbs_.clear();
    negative_ = false;
}

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

    // Almost every literal is short, and running the limb machinery for "2"
    // costs an allocation and a multiply-accumulate loop for nothing. Eighteen
    // digits always fit in 64 bits, so no overflow check is needed.
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

    Limbs limbs;
    // Nine digits at a time: the largest power of ten that cannot overflow the
    // multiply below.
    for (std::size_t at = 0; at < text.size();) {
        const std::size_t chunk = std::min<std::size_t>(9, text.size() - at);
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < chunk; ++i) {
            const char c = text[at + i];
            if (c < '0' || c > '9') {
                return std::nullopt;
            }
            value = value * 10 + static_cast<std::uint32_t>(c - '0');
        }
        std::uint32_t scale = 1;
        for (std::size_t i = 0; i < chunk; ++i) {
            scale *= 10;
        }
        limbs = multiplyMagnitude(limbs, Limbs{scale});
        limbs = addMagnitude(limbs, value == 0 ? Limbs{} : Limbs{value});
        at += chunk;
    }

    Integer result;
    result.limbs_ = std::move(limbs);
    result.negative_ = negative && !result.limbs_.empty();
    result.demote();
    return result;
}

Integer::Integer(std::string_view text) {
    if (auto parsed = parse(text)) {
        *this = std::move(*parsed);
        return;
    }
    throw ParseError("not an integer: '" + std::string(text) + "'");
}

// --- inspection -----------------------------------------------------------

int Integer::sign() const {
    if (limbs_.empty()) {
        return small_ > 0 ? 1 : (small_ < 0 ? -1 : 0);
    }
    return negative_ ? -1 : 1;
}

std::optional<std::int64_t> Integer::toInt64() const {
    if (limbs_.empty()) {
        return small_;
    }
    return std::nullopt;
}

std::string Integer::toString() const {
    if (limbs_.empty()) {
        return std::to_string(small_);
    }

    // Repeatedly divided by the largest power of ten a limb can hold, so each
    // step yields nine decimal digits.
    Limbs remaining = limbs_;
    std::string digits;
    while (!remaining.empty()) {
        const std::uint32_t chunk = divideBySmall(remaining, 1000000000u);
        const std::string piece = std::to_string(chunk);
        if (remaining.empty()) {
            digits.insert(0, piece);
        } else {
            digits.insert(0, std::string(9 - piece.size(), '0') + piece);
        }
    }
    if (digits.empty()) {
        digits = "0";
    }
    return negative_ ? "-" + digits : digits;
}

double Integer::toDouble() const {
    if (limbs_.empty()) {
        return static_cast<double>(small_);
    }
    double value = 0.0;
    for (std::size_t i = limbs_.size(); i-- > 0;) {
        value = value * static_cast<double>(kBase) + static_cast<double>(limbs_[i]);
    }
    return negative_ ? -value : value;
}

std::size_t Integer::hash() const {
    // The invariant earns its keep here. A stored value never fits in 64 bits,
    // so it can never equal an inline one, and the two may be hashed by
    // separate routes. Going through toString() instead — which an earlier
    // version did — allocated a string on every hash, and every expression
    // node is hashed once when it is built.
    if (limbs_.empty()) {
        return std::hash<std::int64_t>{}(small_);
    }

    std::size_t seed = negative_ ? 0x9e3779b97f4a7c15ULL : 0xcbf29ce484222325ULL;
    for (const std::uint32_t limb : limbs_) {
        seed ^= limb;
        seed *= 0x100000001b3ULL;
    }
    return seed;
}

// --- arithmetic -----------------------------------------------------------

Integer Integer::operator-() const {
    if (limbs_.empty() && small_ != kMin) {
        return Integer(-small_);
    }
    Integer result = *this;
    result.promote();
    if (!result.limbs_.empty()) {
        result.negative_ = !result.negative_;
    }
    result.demote();
    return result;
}

Integer Integer::operator+(const Integer &other) const {
    if (limbs_.empty() && other.limbs_.empty()) {
        std::int64_t sum = 0;
        if (!addOverflows(small_, other.small_, sum)) {
            return Integer(sum);
        }
    }

    Integer a = *this;
    Integer b = other;
    a.promote();
    b.promote();

    Integer result;
    if (a.negative_ == b.negative_) {
        result.limbs_ = addMagnitude(a.limbs_, b.limbs_);
        result.negative_ = a.negative_;
    } else {
        const int order = compareMagnitude(a.limbs_, b.limbs_);
        if (order == 0) {
            return Integer(0);
        }
        const Integer &larger = order > 0 ? a : b;
        const Integer &smaller = order > 0 ? b : a;
        result.limbs_ = subtractMagnitude(larger.limbs_, smaller.limbs_);
        result.negative_ = larger.negative_;
    }
    result.negative_ = result.negative_ && !result.limbs_.empty();
    result.demote();
    return result;
}

Integer Integer::operator-(const Integer &other) const {
    return *this + (-other);
}

Integer Integer::operator*(const Integer &other) const {
    if (limbs_.empty() && other.limbs_.empty()) {
        std::int64_t product = 0;
        if (!mulOverflows(small_, other.small_, product)) {
            return Integer(product);
        }
    }

    Integer a = *this;
    Integer b = other;
    a.promote();
    b.promote();

    Integer result;
    result.limbs_ = multiplyMagnitude(a.limbs_, b.limbs_);
    result.negative_ = (a.negative_ != b.negative_) && !result.limbs_.empty();
    result.demote();
    return result;
}

Integer Integer::operator/(const Integer &other) const {
    if (other.isZero()) {
        throw Error("integer division by zero");
    }
    if (limbs_.empty() && other.limbs_.empty()) {
        // The one case 64-bit division cannot express.
        if (!(small_ == kMin && other.small_ == -1)) {
            return Integer(small_ / other.small_);
        }
    }

    Integer a = *this;
    Integer b = other;
    a.promote();
    b.promote();

    Limbs quotient;
    Limbs remainder;
    divideMagnitude(a.limbs_, b.limbs_, quotient, remainder);

    Integer result;
    result.limbs_ = std::move(quotient);
    result.negative_ = (a.negative_ != b.negative_) && !result.limbs_.empty();
    result.demote();
    return result;
}

Integer Integer::operator%(const Integer &other) const {
    if (other.isZero()) {
        throw Error("integer division by zero");
    }
    if (limbs_.empty() && other.limbs_.empty()) {
        if (!(small_ == kMin && other.small_ == -1)) {
            return Integer(small_ % other.small_);
        }
    }

    Integer a = *this;
    Integer b = other;
    a.promote();
    b.promote();

    Limbs quotient;
    Limbs remainder;
    divideMagnitude(a.limbs_, b.limbs_, quotient, remainder);

    Integer result;
    result.limbs_ = std::move(remainder);
    // Truncating division, so the remainder takes the dividend's sign.
    result.negative_ = a.negative_ && !result.limbs_.empty();
    result.demote();
    return result;
}

// --- comparison -----------------------------------------------------------

bool Integer::operator==(const Integer &other) const {
    if (limbs_.empty()) {
        // By the invariant, a stored value does not fit in 64 bits and so
        // cannot equal an inline one.
        return other.limbs_.empty() && small_ == other.small_;
    }
    if (other.limbs_.empty()) {
        return false;
    }
    return negative_ == other.negative_ && limbs_ == other.limbs_;
}

std::strong_ordering Integer::operator<=>(const Integer &other) const {
    if (limbs_.empty() && other.limbs_.empty()) {
        return small_ <=> other.small_;
    }

    const int mine = sign();
    const int theirs = other.sign();
    if (mine != theirs) {
        return mine <=> theirs;
    }

    // Same sign, and at least one does not fit in 64 bits. Comparing
    // magnitudes needs both in stored form, but only the inline one has to be
    // converted.
    const Limbs left = limbs_.empty() ? magnitudeOf(small_) : limbs_;
    const Limbs right
        = other.limbs_.empty() ? magnitudeOf(other.small_) : other.limbs_;

    const int order = compareMagnitude(left, right);
    // Among negatives the larger magnitude is the smaller value.
    return (mine < 0 ? -order : order) <=> 0;
}

// --- free functions -------------------------------------------------------

Integer abs(const Integer &value) {
    return value.isNegative() ? -value : value;
}

Integer gcd(const Integer &a, const Integer &b) {
    // The overwhelmingly common case, and worth reaching before anything is
    // copied: every rational reduction calls this, and almost none of them
    // involve a value past 64 bits.
    if (a.limbs_.empty() && b.limbs_.empty()) {
        // Built from unsigned magnitudes so the extreme negative value needs no
        // special case.
        auto p = static_cast<std::uint64_t>(a.small_);
        auto q = static_cast<std::uint64_t>(b.small_);
        if (a.small_ < 0) {
            p = ~p + 1;
        }
        if (b.small_ < 0) {
            q = ~q + 1;
        }
        while (q != 0) {
            const std::uint64_t r = p % q;
            p = q;
            q = r;
        }
        // The result divides both, so it cannot exceed the smaller magnitude
        // unless one was zero — and zero cannot be the extreme value.
        if (p <= static_cast<std::uint64_t>(kMax)) {
            return Integer(static_cast<std::int64_t>(p));
        }
        return *Integer::parse(std::to_string(p));
    }

    // Binary GCD: shifts and subtraction only, so it never needs the division
    // routine. Euclid's would call it once per step.
    Integer x = abs(a);
    Integer y = abs(b);
    if (x.isZero()) {
        return y;
    }
    if (y.isZero()) {
        return x;
    }

    x.promote();
    y.promote();
    Limbs u = x.limbs_;
    Limbs v = y.limbs_;

    std::size_t shared = 0;
    while (isEvenMagnitude(u) && isEvenMagnitude(v)) {
        shiftRightOne(u);
        shiftRightOne(v);
        ++shared;
    }
    while (isEvenMagnitude(u)) {
        shiftRightOne(u);
    }
    do {
        while (isEvenMagnitude(v)) {
            shiftRightOne(v);
        }
        if (compareMagnitude(u, v) > 0) {
            std::swap(u, v);
        }
        v = subtractMagnitude(v, u);
    } while (!v.empty());

    for (std::size_t i = 0; i < shared; ++i) {
        shiftLeftOne(u);
    }

    Integer result;
    result.limbs_ = std::move(u);
    result.negative_ = false;
    result.demote();
    return result;
}

} // namespace mx
