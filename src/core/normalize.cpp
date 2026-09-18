#include "core/normalize.hpp"

#include "core/big_int.hpp"

#include <proxima/errors.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <span>

// Normalisation, not simplification.
//
// The job here is to make structurally identical expressions *look* identical,
// so that equality and hashing are meaningful and cache keys hit. It is
// deliberately not algebra: `x - x` stays `x - x` rather than collapsing to 0,
// and `(x+1)^2` is never expanded. Collecting like terms, factoring and the
// rest are Maxima's business — one canonicaliser, which is the whole reason
// SymEngine was dropped.

namespace proxima::detail {
namespace {

/// An exact rational accumulator: Boost's cpp_rational, which keeps itself
/// reduced with the sign on the numerator.
///
/// It used to be a hand-written numerator and denominator with its own gcd
/// reduction — a second copy of what Expr::rational does, free to drift from
/// it. Nothing here checks for overflow, because nothing overflows.
class Exact {
public:
    /// Starts at the identity: 1 for a product, 0 for a sum.
    explicit Exact(bool product) : value_(product ? 1 : 0) {}

    void add(const Integer &n, const Integer &d) { value_ += fraction(n, d); }
    void multiply(const Integer &n, const Integer &d) { value_ *= fraction(n, d); }

    Integer numerator() const {
        return IntegerAccess::from_wide(boost::multiprecision::numerator(value_));
    }
    Integer denominator() const {
        return IntegerAccess::from_wide(boost::multiprecision::denominator(value_));
    }

    /// The value as a double, correctly rounded. Dividing the two parts as
    /// doubles, as this once did, rounded twice — and gave NaN once both parts
    /// were beyond a double's range, however ordinary their ratio.
    double approx() const { return value_.convert_to<double>(); }

private:
    static WideRational fraction(const Integer &n, const Integer &d) {
        return WideRational(IntegerAccess::wide(n), IntegerAccess::wide(d));
    }

    WideRational value_;
};

bool is_exact_zero(const Expr &expr) {
    return expr.is(Kind::Integer) && expr.integer_value().is_zero();
}

bool is_exact_one(const Expr &expr) {
    return expr.is(Kind::Integer) && expr.integer_value() == Integer(1);
}

int rank_of(Kind kind) {
    switch (kind) {
    case Kind::Integer:
    case Kind::Rational:
    case Kind::Real:
        return 0;
    case Kind::Symbol:
        return 1;
    case Kind::Function:
        return 2;
    case Kind::Pow:
        return 3;
    case Kind::Mul:
        return 4;
    case Kind::Add:
        return 5;
    case Kind::Relation:
        return 6;
    case Kind::Opaque:
        return 7;
    }
    return 8;
}

int compare_text(const std::string &a, const std::string &b) {
    const int order = a.compare(b);
    return order < 0 ? -1 : (order > 0 ? 1 : 0);
}

int compare_args(const Expr &lhs, const Expr &rhs) {
    if (lhs.arity() != rhs.arity()) {
        return lhs.arity() < rhs.arity() ? -1 : 1;
    }
    for (std::size_t i = 0; i < lhs.arity(); ++i) {
        if (const int order = compare_expr(lhs.arg(i), rhs.arg(i)); order != 0) {
            return order;
        }
    }
    return 0;
}

double approx_value(const Expr &number) {
    switch (number.kind()) {
    case Kind::Integer:
        return number.integer_value().to_double();
    case Kind::Rational:
        return number.numerator().to_double() / number.denominator().to_double();
    default:
        return number.real_value();
    }
}

/// Splices the operands of any nested `kind` node into `flat`.
void flatten_into(const Expr &expr, Kind kind, std::vector<Expr> &flat) {
    if (expr.is(kind)) {
        // Children are normalised already, so one level of splicing is enough.
        for (const Expr &operand : expr.args()) {
            flat.push_back(operand);
        }
    } else {
        flat.push_back(expr);
    }
}

bool canonically_before(const Expr &a, const Expr &b) {
    return compare_expr(a, b) < 0;
}

/// Folds sorted numeric operands into one. Always succeeds: exact arithmetic
/// cannot fail now that proxima::Integer is unbounded.
Expr fold(std::span<const Expr> numbers, bool is_product) {
    // One number is already its own fold, and rebuilding an equal node for it
    // was an allocation on every `x + 1`. The one exception is a lone -0.0 in a
    // sum: the arithmetic below computes 0.0 + -0.0, which is +0.0, and that
    // result is kept.
    if (numbers.size() == 1) {
        const Expr &only = numbers.front();
        const bool negative_zero_in_sum = !is_product && only.is(Kind::Real)
                                       && only.real_value() == 0.0
                                       && std::signbit(only.real_value());
        if (!negative_zero_in_sum) {
            return only;
        }
    }

    Exact exact(is_product);

    bool saw_real = false;
    double inexact = is_product ? 1.0 : 0.0;

    for (const Expr &number : numbers) {
        if (number.is(Kind::Real)) {
            saw_real = true;
            if (is_product) {
                inexact *= number.real_value();
            } else {
                inexact += number.real_value();
            }
            continue;
        }
        if (is_product) {
            exact.multiply(number.numerator(), number.denominator());
        } else {
            exact.add(number.numerator(), number.denominator());
        }
    }

    // Inexactness is contagious, as it is in Maxima: one float makes the whole
    // constant a float.
    if (saw_real) {
        const double folded = is_product ? inexact * exact.approx()
                                        : inexact + exact.approx();
        // No Real operand is infinite, so a result that is not finite is an
        // overflow: 1e308 * 10.0, or an integer of a few hundred digits made a
        // double. It used to become infinity silently; Maxima refuses it too.
        if (!std::isfinite(folded)) {
            throw OverflowError("floating-point overflow: the numbers in this "
                        + std::string(is_product ? "product" : "sum")
                        + " do not fit in a double");
        }
        return Expr::real(folded);
    }
    return Expr::rational(exact.numerator(), exact.denominator());
}

std::vector<Expr> normalize(std::vector<Expr> operands, Kind kind) {
    const bool is_product = kind == Kind::Mul;

    // The shape a loop of `acc = acc + term` makes: a node of this kind, which
    // is canonical already, and one more operand that is neither a number nor
    // of this kind. Its place in the order is a binary search away, where the
    // general path below flattens, partitions and sorts all n operands again.
    // The result is the same — canonical order puts equal operands next to
    // one another however they arrive — and the copy remains, the node being
    // immutable, so a chain is still quadratic; but without the sort.
    if (operands.size() == 2) {
        const bool first_nested = operands[0].is(kind);
        const Expr &nested = first_nested ? operands[0] : operands[1];
        const Expr &other = first_nested ? operands[1] : operands[0];
        if (nested.is(kind) && !other.is(kind) && !other.is_number()) {
            const std::vector<Expr> &existing = nested.args();
            const auto at = std::upper_bound(existing.begin(), existing.end(), other,
                                             canonically_before);
            std::vector<Expr> merged;
            merged.reserve(existing.size() + 1);
            merged.insert(merged.end(), existing.begin(), at);
            merged.push_back(other);
            merged.insert(merged.end(), at, existing.end());
            return merged;
        }
    }

    // Flattened into a new vector only when some operand needs it. Otherwise
    // the caller's vector is already flat, so it is worked on in place and
    // handed back to become the node's operands. `x + y` used to copy its two
    // operands through three more vectors before the node was built.
    const auto nested = [kind](const Expr &operand) { return operand.is(kind); };
    if (std::any_of(operands.begin(), operands.end(), nested)) {
        std::vector<Expr> flat;
        flat.reserve(operands.size());
        for (const Expr &operand : operands) {
            flatten_into(operand, kind, flat);
        }
        operands = std::move(flat);
    }

    // Numbers to the front, and there into canonical order, so that folding
    // them is deterministic — for reals the order changes the result's last
    // bits. std::partition rather than std::stable_partition, which may
    // allocate; no order that matters is lost, since the numbers are sorted
    // here and everything else at the end.
    const auto numbers_end
        = std::partition(operands.begin(), operands.end(),
                         [](const Expr &operand) { return operand.is_number(); });
    if (numbers_end != operands.begin()) {
        std::sort(operands.begin(), numbers_end, canonically_before);
        const auto count = static_cast<std::size_t>(numbers_end - operands.begin());
        Expr constant = fold(std::span<const Expr>(operands.data(), count), is_product);
        operands.erase(operands.begin(), numbers_end);

        if (is_product && is_exact_zero(constant)) {
            // Absorbing, so nothing else matters.
            return {std::move(constant)};
        }
        const bool is_identity
            = is_product ? is_exact_one(constant) : is_exact_zero(constant);
        // The identity is dropped, unless it is all that is left — `0` has to
        // remain `0`.
        if (!is_identity || operands.empty()) {
            operands.push_back(std::move(constant));
        }
    }

    std::sort(operands.begin(), operands.end(), canonically_before);
    return operands;
}

} // namespace

int compare_expr(const Expr &lhs, const Expr &rhs) {
    const int left_rank = rank_of(lhs.kind());
    const int right_rank = rank_of(rhs.kind());

    if (lhs.is_number() && rhs.is_number()) {
        // Doubles first, since they are cheap and nearly always decisive.
        // Rounding never reverses an order, so unequal doubles mean the numbers
        // themselves compare the same way.
        const double a = approx_value(lhs);
        const double b = approx_value(rhs);
        if (a < b) {
            return -1;
        }
        if (a > b) {
            return 1;
        }

        if (!lhs.is(Kind::Real) && !rhs.is(Kind::Real)) {
            // Equal as doubles, but two exact numbers can still differ: 2^100 and
            // 2^100 + 1 round to the same double. This used to stop at the
            // doubles and call them equal although == told them apart, and an
            // ordered container keyed on the order would have merged them. So
            // compare exactly, by cross-multiplying — denominators are positive,
            // so the sign of the difference survives. Equal values are the same
            // number: an Integer and a Rational are never equal.
            const Integer left = lhs.numerator() * rhs.denominator();
            const Integer right = rhs.numerator() * lhs.denominator();
            if (left < right) {
                return -1;
            }
            return right < left ? 1 : 0;
        }

        // Equal in value with a real involved: order by kind, so that 2 and 2.0
        // still have a stable relative order. That always puts an exact number
        // before a real of the same value, which with rounding's monotonicity
        // keeps the order consistent. Two reals equal in value — 0.0 and -0.0
        // included — are equal expressions too.
        if (lhs.kind() != rhs.kind()) {
            return static_cast<int>(lhs.kind()) < static_cast<int>(rhs.kind()) ? -1
                                                                              : 1;
        }
        return 0;
    }

    if (left_rank != right_rank) {
        return left_rank < right_rank ? -1 : 1;
    }

    switch (lhs.kind()) {
    case Kind::Symbol:
        return compare_text(lhs.name(), rhs.name());
    case Kind::Opaque:
        return compare_text(lhs.opaque_text(), rhs.opaque_text());
    case Kind::Function:
        if (const int order = compare_text(lhs.name(), rhs.name()); order != 0) {
            return order;
        }
        return compare_args(lhs, rhs);
    case Kind::Relation:
        if (lhs.relation_op() != rhs.relation_op()) {
            return static_cast<int>(lhs.relation_op())
                           < static_cast<int>(rhs.relation_op())
                       ? -1
                       : 1;
        }
        return compare_args(lhs, rhs);
    default:
        return compare_args(lhs, rhs);
    }
}

std::vector<Expr> normalize_sum(std::vector<Expr> terms) {
    return normalize(std::move(terms), Kind::Add);
}

std::vector<Expr> normalize_product(std::vector<Expr> factors) {
    return normalize(std::move(factors), Kind::Mul);
}

std::optional<Expr> normalize_power(const Expr &base, const Expr &exponent) {
    if (is_exact_one(exponent)) {
        return base;
    }
    if (is_exact_zero(exponent)) {
        // 0^0 is left alone: Maxima has its own opinion and this is not the
        // place to pre-empt it.
        if (!is_exact_zero(base)) {
            return Expr::integer(1);
        }
        return std::nullopt;
    }
    if (is_exact_one(base)) {
        return Expr::integer(1);
    }
    // A reciprocal is kept in one form, the one division produces and the
    // printer writes: found by fuzzing, 2^-1 printed as 1/2, which reads back as
    // the Rational, and s*n^-1*f^-1 printed as s/(n*f), which read back as
    // s*(n*f)^-1. So an exact number's reciprocal is the Rational — as 2/4 is
    // already 1/2, though 2^3 is still not 8 — and a product's reciprocal is
    // the product of its factors' reciprocals, as Maxima keeps a quotient.
    if (exponent.is(Kind::Integer) && exponent.integer_value() == Integer(-1)) {
        if (base.is(Kind::Integer) && !base.integer_value().is_zero()) {
            return Expr::rational(Integer(1), base.integer_value());
        }
        if (base.is(Kind::Rational)) {
            return Expr::rational(base.denominator(), base.numerator());
        }
        if (base.is(Kind::Mul)) {
            std::vector<Expr> reciprocals;
            reciprocals.reserve(base.arity());
            for (const Expr &factor : base.args()) {
                reciprocals.push_back(Expr::pow(factor, exponent));
            }
            return Expr::mul(std::move(reciprocals));
        }
    }
    // (a^r)^n is a^(r*n) when n is an integer, whatever a and r are: an
    // integer power has one value, so nothing is lost. Maxima simplifies it so
    // on every result. Found by fuzzing: without it `1/x^2`, which is
    // (x^2)^-1, was a different expression from x^-2 — which is exactly how the
    // printer writes x^-2, so a printed expression did not read back.
    if (base.is(Kind::Pow) && exponent.is(Kind::Integer)) {
        return Expr::pow(base.arg(0), Expr::mul({base.arg(1), exponent}));
    }
    return std::nullopt;
}

} // namespace proxima::detail
