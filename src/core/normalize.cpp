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

constexpr Integer kMin = std::numeric_limits<Integer>::min();
constexpr Integer kMax = std::numeric_limits<Integer>::max();

bool addOverflows(Integer a, Integer b, Integer &out) {
    if (b > 0 && a > kMax - b) {
        return true;
    }
    if (b < 0 && a < kMin - b) {
        return true;
    }
    out = a + b;
    return false;
}

bool mulOverflows(Integer a, Integer b, Integer &out) {
    if (a == 0 || b == 0) {
        out = 0;
        return false;
    }
    // Handled separately because kMin has no positive counterpart.
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

/// An exact rational accumulator that refuses to wrap.
struct Exact {
    Integer numerator = 0;
    Integer denominator = 1;

    bool add(Integer n, Integer d) {
        Integer left = 0, right = 0, sum = 0, product = 0;
        if (mulOverflows(numerator, d, left) || mulOverflows(n, denominator, right)
            || addOverflows(left, right, sum)
            || mulOverflows(denominator, d, product)) {
            return false;
        }
        numerator = sum;
        denominator = product;
        reduce();
        return true;
    }

    bool multiply(Integer n, Integer d) {
        Integer top = 0, bottom = 0;
        if (mulOverflows(numerator, n, top)
            || mulOverflows(denominator, d, bottom)) {
            return false;
        }
        numerator = top;
        denominator = bottom;
        reduce();
        return true;
    }

    void reduce() {
        // Keeps the accumulator small, which is what stops a long sum of
        // fractions overflowing on the denominators alone.
        const Integer divisor = std::gcd(numerator, denominator);
        if (divisor > 1) {
            numerator /= divisor;
            denominator /= divisor;
        }
    }

    double approx() const {
        return static_cast<double>(numerator) / static_cast<double>(denominator);
    }
};

bool isExactZero(const Expr &expr) {
    return expr.is(Kind::Integer) && expr.integerValue() == 0;
}

bool isExactOne(const Expr &expr) {
    return expr.is(Kind::Integer) && expr.integerValue() == 1;
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
        return static_cast<double>(number.integerValue());
    case Kind::Rational:
        return static_cast<double>(number.numerator())
               / static_cast<double>(number.denominator());
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

/// Folds sorted numeric operands into one.
///
/// Returns nullopt if exact arithmetic would overflow mx::Integer, in which
/// case the caller keeps the numbers unfolded — correct, if less tidy, and far
/// better than wrapping.
std::optional<Expr> fold(const std::vector<Expr> &numbers, bool isProduct) {
    Exact exact;
    if (isProduct) {
        exact.numerator = 1;
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
        const bool ok = isProduct
                            ? exact.multiply(number.numerator(),
                                             number.denominator())
                            : exact.add(number.numerator(),
                                        number.denominator());
        if (!ok) {
            return std::nullopt;
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

    std::optional<Expr> constant;
    if (!numbers.empty()) {
        constant = fold(numbers, isProduct);
    }

    if (constant) {
        if (isProduct && isExactZero(*constant)) {
            return {Expr::integer(0)}; // Absorbing, so nothing else matters.
        }
        const bool isIdentity
            = isProduct ? isExactOne(*constant) : isExactZero(*constant);
        // The identity is dropped, unless it is all that is left — `0` has to
        // remain `0`.
        if (!isIdentity || flat.empty()) {
            flat.push_back(*constant);
        }
    } else {
        // Folding overflowed; keep the numbers as they were.
        flat.insert(flat.end(), numbers.begin(), numbers.end());
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
