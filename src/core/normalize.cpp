#include "core/normalize.hpp"

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

/// An exact rational accumulator.
///
/// Nothing here checks for overflow, because proxima::Integer has none. Before it
/// was unbounded this was three checked helpers and a failure path that left
/// the terms unfolded.
struct Exact {
    Integer numerator{0};
    Integer denominator{1};

    void add(const Integer &n, const Integer &d) {
        numerator = numerator * d + n * denominator;
        denominator = denominator * d;
        reduce();
    }

    void multiply(const Integer &n, const Integer &d) {
        numerator = numerator * n;
        denominator = denominator * d;
        reduce();
    }

    void reduce() {
        // Keeps the accumulator small, which matters more now that it *can*
        // grow without bound: a long sum of fractions would otherwise carry an
        // ever-larger product of denominators.
        const Integer divisor = gcd(numerator, denominator);
        if (divisor > Integer(1)) {
            numerator = numerator / divisor;
            denominator = denominator / divisor;
        }
    }

    double approx() const {
        return numerator.toDouble() / denominator.toDouble();
    }
};

bool isExactZero(const Expr &expr) {
    return expr.is(Kind::Integer) && expr.integerValue().isZero();
}

bool isExactOne(const Expr &expr) {
    return expr.is(Kind::Integer) && expr.integerValue() == Integer(1);
}

int rankOf(Kind kind) {
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

int compareText(const std::string &a, const std::string &b) {
    const int order = a.compare(b);
    return order < 0 ? -1 : (order > 0 ? 1 : 0);
}

int compareArgs(const Expr &lhs, const Expr &rhs) {
    if (lhs.arity() != rhs.arity()) {
        return lhs.arity() < rhs.arity() ? -1 : 1;
    }
    for (std::size_t i = 0; i < lhs.arity(); ++i) {
        if (const int order = compareExpr(lhs.arg(i), rhs.arg(i)); order != 0) {
            return order;
        }
    }
    return 0;
}

double approxValue(const Expr &number) {
    switch (number.kind()) {
    case Kind::Integer:
        return number.integerValue().toDouble();
    case Kind::Rational:
        return number.numerator().toDouble() / number.denominator().toDouble();
    default:
        return number.realValue();
    }
}

/// Splices the operands of any nested `kind` node into `flat`.
void flattenInto(const Expr &expr, Kind kind, std::vector<Expr> &flat) {
    if (expr.is(kind)) {
        // Children are normalised already, so one level of splicing is enough.
        for (const Expr &operand : expr.args()) {
            flat.push_back(operand);
        }
    } else {
        flat.push_back(expr);
    }
}

bool canonicallyBefore(const Expr &a, const Expr &b) {
    return compareExpr(a, b) < 0;
}

/// Folds sorted numeric operands into one. Always succeeds: exact arithmetic
/// cannot fail now that proxima::Integer is unbounded.
Expr fold(std::span<const Expr> numbers, bool isProduct) {
    // One number is already its own fold, and rebuilding an equal node for it
    // was an allocation on every `x + 1`. The one exception is a lone -0.0 in a
    // sum: the arithmetic below computes 0.0 + -0.0, which is +0.0, and that
    // result is kept.
    if (numbers.size() == 1) {
        const Expr &only = numbers.front();
        const bool negativeZeroInSum = !isProduct && only.is(Kind::Real)
                                       && only.realValue() == 0.0
                                       && std::signbit(only.realValue());
        if (!negativeZeroInSum) {
            return only;
        }
    }

    Exact exact;
    if (isProduct) {
        exact.numerator = Integer(1);
    }

    bool sawReal = false;
    double inexact = isProduct ? 1.0 : 0.0;

    for (const Expr &number : numbers) {
        if (number.is(Kind::Real)) {
            sawReal = true;
            if (isProduct) {
                inexact *= number.realValue();
            } else {
                inexact += number.realValue();
            }
            continue;
        }
        if (isProduct) {
            exact.multiply(number.numerator(), number.denominator());
        } else {
            exact.add(number.numerator(), number.denominator());
        }
    }

    // Inexactness is contagious, as it is in Maxima: one float makes the whole
    // constant a float.
    if (sawReal) {
        const double folded = isProduct ? inexact * exact.approx()
                                        : inexact + exact.approx();
        // No Real operand is infinite, so a result that is not finite is an
        // overflow: 1e308 * 10.0, or an integer of a few hundred digits made a
        // double. It used to become infinity silently; Maxima refuses it too.
        if (!std::isfinite(folded)) {
            throw Error("floating-point overflow: the numbers in this "
                        + std::string(isProduct ? "product" : "sum")
                        + " do not fit in a double");
        }
        return Expr::real(folded);
    }
    return Expr::rational(exact.numerator, exact.denominator);
}

std::vector<Expr> normalize(std::vector<Expr> operands, Kind kind) {
    const bool isProduct = kind == Kind::Mul;

    // Flattened into a new vector only when some operand needs it. Otherwise
    // the caller's vector is already flat, so it is worked on in place and
    // handed back to become the node's operands. `x + y` used to copy its two
    // operands through three more vectors before the node was built.
    const auto nested = [kind](const Expr &operand) { return operand.is(kind); };
    if (std::any_of(operands.begin(), operands.end(), nested)) {
        std::vector<Expr> flat;
        flat.reserve(operands.size());
        for (const Expr &operand : operands) {
            flattenInto(operand, kind, flat);
        }
        operands = std::move(flat);
    }

    // Numbers to the front, and there into canonical order, so that folding
    // them is deterministic — for reals the order changes the result's last
    // bits. std::partition rather than std::stable_partition, which may
    // allocate; no order that matters is lost, since the numbers are sorted
    // here and everything else at the end.
    const auto numbersEnd
        = std::partition(operands.begin(), operands.end(),
                         [](const Expr &operand) { return operand.isNumber(); });
    if (numbersEnd != operands.begin()) {
        std::sort(operands.begin(), numbersEnd, canonicallyBefore);
        const auto count = static_cast<std::size_t>(numbersEnd - operands.begin());
        Expr constant = fold(std::span<const Expr>(operands.data(), count), isProduct);
        operands.erase(operands.begin(), numbersEnd);

        if (isProduct && isExactZero(constant)) {
            // Absorbing, so nothing else matters.
            return {std::move(constant)};
        }
        const bool isIdentity
            = isProduct ? isExactOne(constant) : isExactZero(constant);
        // The identity is dropped, unless it is all that is left — `0` has to
        // remain `0`.
        if (!isIdentity || operands.empty()) {
            operands.push_back(std::move(constant));
        }
    }

    std::sort(operands.begin(), operands.end(), canonicallyBefore);
    return operands;
}

} // namespace

int compareExpr(const Expr &lhs, const Expr &rhs) {
    const int leftRank = rankOf(lhs.kind());
    const int rightRank = rankOf(rhs.kind());

    if (lhs.isNumber() && rhs.isNumber()) {
        // Doubles first, since they are cheap and nearly always decisive.
        // Rounding never reverses an order, so unequal doubles mean the numbers
        // themselves compare the same way.
        const double a = approxValue(lhs);
        const double b = approxValue(rhs);
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

    if (leftRank != rightRank) {
        return leftRank < rightRank ? -1 : 1;
    }

    switch (lhs.kind()) {
    case Kind::Symbol:
        return compareText(lhs.name(), rhs.name());
    case Kind::Opaque:
        return compareText(lhs.opaqueText(), rhs.opaqueText());
    case Kind::Function:
        if (const int order = compareText(lhs.name(), rhs.name()); order != 0) {
            return order;
        }
        return compareArgs(lhs, rhs);
    case Kind::Relation:
        if (lhs.relationOp() != rhs.relationOp()) {
            return static_cast<int>(lhs.relationOp())
                           < static_cast<int>(rhs.relationOp())
                       ? -1
                       : 1;
        }
        return compareArgs(lhs, rhs);
    default:
        return compareArgs(lhs, rhs);
    }
}

std::vector<Expr> normalizeSum(std::vector<Expr> terms) {
    return normalize(std::move(terms), Kind::Add);
}

std::vector<Expr> normalizeProduct(std::vector<Expr> factors) {
    return normalize(std::move(factors), Kind::Mul);
}

std::optional<Expr> normalizePower(const Expr &base, const Expr &exponent) {
    if (isExactOne(exponent)) {
        return base;
    }
    if (isExactZero(exponent)) {
        // 0^0 is left alone: Maxima has its own opinion and this is not the
        // place to pre-empt it.
        if (!isExactZero(base)) {
            return Expr::integer(1);
        }
        return std::nullopt;
    }
    if (isExactOne(base)) {
        return Expr::integer(1);
    }
    // A reciprocal is kept in one form, the one division produces and the
    // printer writes: found by fuzzing, 2^-1 printed as 1/2, which reads back as
    // the Rational, and s*n^-1*f^-1 printed as s/(n*f), which read back as
    // s*(n*f)^-1. So an exact number's reciprocal is the Rational — as 2/4 is
    // already 1/2, though 2^3 is still not 8 — and a product's reciprocal is
    // the product of its factors' reciprocals, as Maxima keeps a quotient.
    if (exponent.is(Kind::Integer) && exponent.integerValue() == Integer(-1)) {
        if (base.is(Kind::Integer) && !base.integerValue().isZero()) {
            return Expr::rational(Integer(1), base.integerValue());
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
