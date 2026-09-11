#include "core/normalize.hpp"

#include <algorithm>
#include <limits>
#include <numeric>

// Normalisation, not simplification.
//
// The job here is to make structurally identical expressions *look* identical,
// so that equality and hashing are meaningful and cache keys hit. It is
// deliberately not algebra: `x - x` stays `x - x` rather than collapsing to 0,
// and `(x+1)^2` is never expanded. Collecting like terms, factoring and the
// rest are Maxima's business — one canonicaliser, which is the whole reason
// SymEngine was dropped.

namespace mx::detail {
namespace {

/// An exact rational accumulator.
///
/// Nothing here checks for overflow, because mx::Integer has none. Before it
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

/// Splits `operands` into its numeric and non-numeric parts, leaving the
/// numbers in canonical order so that folding them is deterministic.
std::vector<Expr> partitionNumbers(std::vector<Expr> &operands) {
    std::vector<Expr> numbers;
    std::vector<Expr> rest;
    for (Expr &operand : operands) {
        if (operand.isNumber()) {
            numbers.push_back(std::move(operand));
        } else {
            rest.push_back(std::move(operand));
        }
    }
    std::sort(numbers.begin(), numbers.end(),
              [](const Expr &a, const Expr &b) { return compareExpr(a, b) < 0; });
    operands = std::move(rest);
    return numbers;
}

/// Folds sorted numeric operands into one. Always succeeds: exact arithmetic
/// cannot fail now that mx::Integer is unbounded.
Expr fold(const std::vector<Expr> &numbers, bool isProduct) {
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
        return Expr::real(isProduct ? inexact * exact.approx()
                                    : inexact + exact.approx());
    }
    return Expr::rational(exact.numerator, exact.denominator);
}

std::vector<Expr> normalize(std::vector<Expr> operands, Kind kind) {
    const bool isProduct = kind == Kind::Mul;

    std::vector<Expr> flat;
    flat.reserve(operands.size());
    for (const Expr &operand : operands) {
        flattenInto(operand, kind, flat);
    }

    std::vector<Expr> numbers = partitionNumbers(flat);

    if (!numbers.empty()) {
        const Expr constant = fold(numbers, isProduct);
        if (isProduct && isExactZero(constant)) {
            return {Expr::integer(0)}; // Absorbing, so nothing else matters.
        }
        const bool isIdentity
            = isProduct ? isExactOne(constant) : isExactZero(constant);
        // The identity is dropped, unless it is all that is left — `0` has to
        // remain `0`.
        if (!isIdentity || flat.empty()) {
            flat.push_back(constant);
        }
    }

    std::sort(flat.begin(), flat.end(),
              [](const Expr &a, const Expr &b) { return compareExpr(a, b) < 0; });
    return flat;
}

} // namespace

int compareExpr(const Expr &lhs, const Expr &rhs) {
    const int leftRank = rankOf(lhs.kind());
    const int rightRank = rankOf(rhs.kind());

    if (lhs.isNumber() && rhs.isNumber()) {
        const double a = approxValue(lhs);
        const double b = approxValue(rhs);
        if (a < b) {
            return -1;
        }
        if (a > b) {
            return 1;
        }
        // Equal in magnitude: order by kind so that 2, 2/1 and 2.0 still have a
        // stable relative order.
        if (lhs.kind() != rhs.kind()) {
            return static_cast<int>(lhs.kind()) < static_cast<int>(rhs.kind()) ? -1
                                                                              : 1;
        }
        if (lhs.is(Kind::Rational)) {
            if (lhs.denominator() != rhs.denominator()) {
                return lhs.denominator() < rhs.denominator() ? -1 : 1;
            }
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
    return std::nullopt;
}

} // namespace mx::detail
