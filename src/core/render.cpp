#include <mx/render.hpp>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

// Layer one: the algebraic tree becomes a *display* tree.
//
// This is the part every renderer would otherwise re-derive, and the part
// everyone gets wrong. Writing a TeX renderer against the raw Expr as an
// experiment produced seven defects in under two hundred lines — `x - 1`
// printing as `-1 + x`, `x/3` as a product containing a fraction, `1/x` as a
// negative power, `-(x+1)` as `(-1)*(1+x)` — every one of them a presentation
// decision rather than a question about TeX. They are made once, here.
//
// What this is not: algebra. Nothing below changes the value of anything. A
// Rational becomes a Fraction and `x^(1/2)` becomes a Root because those are
// the same number spelled for a reader; `x - x` still has two terms.

namespace mx::detail {
namespace {

DisplayNode leaf(DisplayKind kind) {
    DisplayNode node;
    node.kind = kind;
    return node;
}

DisplayNode integerNode(Integer value) {
    DisplayNode node = leaf(DisplayKind::Integer);
    node.integer = std::move(value);
    return node;
}

DisplayNode compound(DisplayKind kind, std::vector<DisplayNode> children) {
    DisplayNode node = leaf(kind);
    node.children = std::move(children);
    return node;
}

/// A display node plus the sign that was pulled out of it.
///
/// Extracting the sign is what lets a sum read `a - b` instead of `a + (-b)`,
/// and what turns a top-level `-(x+1)` into a negation rather than a product
/// with a `-1` in it.
struct Signed {
    DisplayNode node;
    bool negated = false;
};

Signed signedDisplay(const Expr &expr);

DisplayNode display(const Expr &expr) {
    Signed value = signedDisplay(expr);
    if (!value.negated) {
        return std::move(value.node);
    }
    std::vector<DisplayNode> child;
    child.push_back(std::move(value.node));
    return compound(DisplayKind::Negate, std::move(child));
}

/// The magnitude of a negative numeric literal, as an Expr.
///
/// Only ever applied to atoms, so it builds nothing compound and cannot
/// disturb canonical order.
Expr magnitudeOf(const Expr &number) {
    switch (number.kind()) {
    case Kind::Integer:
        return Expr::integer(-number.integerValue());
    case Kind::Rational:
        return Expr::rational(-number.numerator(), number.denominator());
    default:
        return Expr::real(-number.realValue());
    }
}

DisplayNode fromPower(const Expr &base, const Expr &exponent);

/// `base^|exponent|`, for an exponent already known to be negative.
DisplayNode reciprocalBody(const Expr &base, const Expr &exponent) {
    // x^-1 is simply x underneath; anything else keeps its exponent.
    if (exponent.is(Kind::Integer) && exponent.integerValue() == Integer(-1)) {
        return display(base);
    }
    return fromPower(base, magnitudeOf(exponent));
}

DisplayNode fromPower(const Expr &base, const Expr &exponent) {
    // x^(1/n) is a root. Bounded because the index is rendered as a small
    // ornament; a denominator past this is better left as a power.
    if (exponent.is(Kind::Rational) && exponent.numerator() == Integer(1)) {
        if (const auto index = exponent.denominator().toInt64();
            index && *index >= 2 && *index <= 64) {
            DisplayNode node = leaf(DisplayKind::Root);
            node.index = static_cast<unsigned>(*index);
            node.children.push_back(display(base));
            return node;
        }
    }

    // A negative exponent is a reciprocal, which reads as a fraction. Without
    // this, `1/x` renders as `x^-1`. Not when the base is a number, though: see
    // fromProduct.
    if (exponent.isNegativeNumber() && !base.isNumber()) {
        std::vector<DisplayNode> parts;
        parts.push_back(integerNode(Integer(1)));
        parts.push_back(reciprocalBody(base, exponent));
        return compound(DisplayKind::Fraction, std::move(parts));
    }

    std::vector<DisplayNode> parts;
    parts.push_back(display(base));
    parts.push_back(display(exponent));
    return compound(DisplayKind::Power, std::move(parts));
}

/// Collapses a list of display nodes into one factor, a product, or the
/// implicit 1 an empty numerator stands for.
DisplayNode joinFactors(std::vector<DisplayNode> factors) {
    if (factors.empty()) {
        return integerNode(Integer(1));
    }
    if (factors.size() == 1) {
        return std::move(factors.front());
    }
    return compound(DisplayKind::Product, std::move(factors));
}

/// A product, split into the parts that belong above and below the line.
///
/// Three things happen here, and all three are visible in the output: the
/// overall sign comes out, negative powers move to the denominator, and a
/// rational coefficient is dismantled so that `x/3` is a fraction containing
/// `x` rather than a product containing `1/3`.
Signed fromProduct(const std::vector<Expr> &args) {
    Signed result;

    std::vector<Expr> factors = args;
    if (!factors.empty() && factors.front().isNegativeNumber()) {
        result.negated = true;
        const Expr magnitude = magnitudeOf(factors.front());
        // A coefficient of exactly -1 is pure sign and leaves nothing behind.
        if (magnitude.is(Kind::Integer)
            && magnitude.integerValue() == Integer(1)) {
            factors.erase(factors.begin());
        } else {
            factors.front() = magnitude;
        }
    }

    std::vector<DisplayNode> above;
    std::vector<DisplayNode> below;
    for (const Expr &factor : factors) {
        // A reciprocal of a number stays a power. Below the line it would be
        // multiplied with the coefficient's denominator and folded when read
        // back: found by fuzzing, 4/269 * 0^-1 printed as 4/(269*0), which
        // reads back as a division by the Integer 0, and 2.5^-1 as 269*2.5
        // would fold to 672.5. An exact non-zero number's reciprocal is already
        // a Rational, so only zero, a real, or a power other than -1 get here.
        if (factor.is(Kind::Pow) && factor.arg(1).isNegativeNumber()
            && !factor.arg(0).isNumber()) {
            below.push_back(reciprocalBody(factor.arg(0), factor.arg(1)));
            continue;
        }
        if (factor.is(Kind::Rational)) {
            // The numerator only earns a place if it is not the implicit 1.
            if (factor.numerator() != Integer(1)) {
                above.push_back(integerNode(factor.numerator()));
            }
            below.push_back(integerNode(factor.denominator()));
            continue;
        }
        above.push_back(display(factor));
    }

    if (below.empty()) {
        result.node = joinFactors(std::move(above));
        return result;
    }

    std::vector<DisplayNode> parts;
    parts.push_back(joinFactors(std::move(above)));
    parts.push_back(joinFactors(std::move(below)));
    result.node = compound(DisplayKind::Fraction, std::move(parts));
    return result;
}

DisplayNode fromSum(const std::vector<Expr> &args) {
    std::vector<Expr> terms = args;

    // Canonical order puts the numeric term first, so `x - 1` arrives as
    // `-1 + x`. Moving a *negative* leading constant to the end restores the
    // conventional reading. This is a display choice and changes nothing about
    // the expression; a positive constant stays put, since `1 - x` already
    // reads better than `-x + 1`.
    if (terms.size() > 1 && terms.front().isNegativeNumber()) {
        std::rotate(terms.begin(), terms.begin() + 1, terms.end());
    }

    DisplayNode node = leaf(DisplayKind::Sum);
    node.children.reserve(terms.size());
    node.negated.reserve(terms.size());
    for (const Expr &term : terms) {
        Signed part = signedDisplay(term);
        node.children.push_back(std::move(part.node));
        node.negated.push_back(part.negated);
    }
    return node;
}

Signed signedDisplay(const Expr &expr) {
    Signed result;

    switch (expr.kind()) {
    case Kind::Integer:
        result.negated = expr.integerValue().isNegative();
        result.node = integerNode(result.negated ? -expr.integerValue()
                                                 : expr.integerValue());
        return result;

    case Kind::Real:
        // The sign bit, not `< 0`: -0.0 is not below zero but prints with a
        // minus. Found by fuzzing: kept inside the number, that minus escaped
        // the bracketing a negative base gets, and (-0.0)^-1 printed as
        // -0.0^(-1), which reads back as -(0.0^-1).
        result.negated = std::signbit(expr.realValue());
        result.node = leaf(DisplayKind::Real);
        result.node.real = std::fabs(expr.realValue());
        return result;

    case Kind::Rational: {
        result.negated = expr.numerator().isNegative();
        std::vector<DisplayNode> parts;
        parts.push_back(integerNode(result.negated ? -expr.numerator()
                                                   : expr.numerator()));
        parts.push_back(integerNode(expr.denominator()));
        result.node = compound(DisplayKind::Fraction, std::move(parts));
        return result;
    }

    case Kind::Symbol:
        result.node = leaf(DisplayKind::Symbol);
        result.node.text = expr.name();
        return result;

    case Kind::Opaque:
        result.node = leaf(DisplayKind::Verbatim);
        result.node.text = expr.opaqueText();
        return result;

    case Kind::Add:
        result.node = fromSum(expr.args());
        return result;

    case Kind::Mul:
        return fromProduct(expr.args());

    case Kind::Pow:
        result.node = fromPower(expr.arg(0), expr.arg(1));
        return result;

    case Kind::Function: {
        std::vector<DisplayNode> args;
        args.reserve(expr.arity());
        for (const Expr &argument : expr.args()) {
            args.push_back(display(argument));
        }
        // Maxima has no textual `list(...)`; brackets are the only spelling,
        // so this head gets its own construct rather than being a call whose
        // name every renderer would have to special-case.
        if (expr.name() == "list") {
            result.node = compound(DisplayKind::List, std::move(args));
            return result;
        }
        result.node = compound(DisplayKind::Call, std::move(args));
        result.node.text = expr.name();
        return result;
    }

    case Kind::Relation: {
        std::vector<DisplayNode> sides;
        sides.push_back(display(expr.arg(0)));
        sides.push_back(display(expr.arg(1)));
        result.node = compound(DisplayKind::Relation, std::move(sides));
        result.node.relOp = expr.relationOp();
        return result;
    }
    }

    result.node = leaf(DisplayKind::Verbatim);
    return result;
}

} // namespace

DisplayNode toDisplay(const Expr &expr) {
    return display(expr);
}

} // namespace mx::detail
