#include <proxima/render.hpp>

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

namespace proxima::detail {
namespace {

DisplayNode integer_node(Integer value) {
    return display_node(DisplayInteger{std::move(value)});
}

/// A display node plus the sign that was pulled out of it.
///
/// Extracting the sign is what lets a sum read `a - b` instead of `a + (-b)`,
/// and what turns a top-level `-(x+1)` into a negation rather than a product
/// with a `-1` in it. It is exactly one addend of a sum, which is what
/// DisplaySum is made of.
using Signed = DisplayTerm;

Signed signed_display(const Expr &expr);

DisplayNode display(const Expr &expr) {
    Signed value = signed_display(expr);
    if (!value.negated) {
        return std::move(value.node);
    }
    return display_node(DisplayNegate(std::move(value.node)));
}

/// The magnitude of a negative numeric literal, as an Expr.
///
/// Only ever applied to atoms, so it builds nothing compound and cannot
/// disturb canonical order.
Expr magnitude_of(const Expr &number) {
    switch (number.kind()) {
    case Kind::Integer:
        return Expr::integer(-number.integer_value());
    case Kind::Rational:
        return Expr::rational(-number.numerator(), number.denominator());
    default:
        return Expr::real(-number.real_value());
    }
}

DisplayNode from_power(const Expr &base, const Expr &exponent);

/// `base^|exponent|`, for an exponent already known to be negative.
DisplayNode reciprocal_body(const Expr &base, const Expr &exponent) {
    // x^-1 is simply x underneath; anything else keeps its exponent.
    if (exponent.is(Kind::Integer) && exponent.integer_value() == Integer(-1)) {
        return display(base);
    }
    return from_power(base, magnitude_of(exponent));
}

DisplayNode from_power(const Expr &base, const Expr &exponent) {
    // x^(1/n) is a root. Bounded because the index is rendered as a small
    // ornament; a denominator past this is better left as a power.
    if (exponent.is(Kind::Rational) && exponent.numerator() == Integer(1)) {
        if (const auto index = exponent.denominator().to_int64();
            index && *index >= 2 && *index <= 64) {
            return display_node(
                DisplayRoot(display(base), static_cast<unsigned>(*index)));
        }
    }

    // A negative exponent is a reciprocal, which reads as a fraction. Without
    // this, `1/x` renders as `x^-1`. Not when the base is a number, though: see
    // from_product.
    if (exponent.is_negative_number() && !base.is_number()) {
        return display_node(DisplayFraction(integer_node(Integer(1)),
                                            reciprocal_body(base, exponent)));
    }

    return display_node(DisplayPower(display(base), display(exponent)));
}

/// Collapses a list of display nodes into one factor, a product, or the
/// implicit 1 an empty numerator stands for.
DisplayNode join_factors(std::vector<DisplayNode> factors) {
    if (factors.empty()) {
        return integer_node(Integer(1));
    }
    if (factors.size() == 1) {
        return std::move(factors.front());
    }
    return display_node(DisplayProduct{std::move(factors)});
}

/// A product, split into the parts that belong above and below the line.
///
/// Three things happen here, and all three are visible in the output: the
/// overall sign comes out, negative powers move to the denominator, and a
/// rational coefficient is dismantled so that `x/3` is a fraction containing
/// `x` rather than a product containing `1/3`.
Signed from_product(const std::vector<Expr> &args) {
    Signed result;

    std::vector<Expr> factors = args;
    if (!factors.empty() && factors.front().is_negative_number()) {
        result.negated = true;
        const Expr magnitude = magnitude_of(factors.front());
        // A coefficient of exactly -1 is pure sign and leaves nothing behind.
        if (magnitude.is(Kind::Integer) && magnitude.integer_value() == Integer(1)) {
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
        if (factor.is(Kind::Pow) && factor.arg(1).is_negative_number()
            && !factor.arg(0).is_number()) {
            below.push_back(reciprocal_body(factor.arg(0), factor.arg(1)));
            continue;
        }
        if (factor.is(Kind::Rational)) {
            // The numerator only earns a place if it is not the implicit 1.
            if (factor.numerator() != Integer(1)) {
                above.push_back(integer_node(factor.numerator()));
            }
            below.push_back(integer_node(factor.denominator()));
            continue;
        }
        above.push_back(display(factor));
    }

    if (below.empty()) {
        result.node = join_factors(std::move(above));
        return result;
    }

    result.node = display_node(DisplayFraction(join_factors(std::move(above)),
                                               join_factors(std::move(below))));
    return result;
}

DisplayNode from_sum(const std::vector<Expr> &args) {
    std::vector<Expr> terms = args;

    // Canonical order puts the numeric term first, so `x - 1` arrives as
    // `-1 + x`. Moving a *negative* leading constant to the end restores the
    // conventional reading. This is a display choice and changes nothing about
    // the expression; a positive constant stays put, since `1 - x` already
    // reads better than `-x + 1`.
    if (terms.size() > 1 && terms.front().is_negative_number()) {
        std::rotate(terms.begin(), terms.begin() + 1, terms.end());
    }

    // Each addend carries its own sign, so the terms and the signs can no
    // longer be of different lengths.
    DisplaySum sum;
    sum.terms.reserve(terms.size());
    for (const Expr &term : terms) {
        sum.terms.push_back(signed_display(term));
    }
    return display_node(std::move(sum));
}

Signed signed_display(const Expr &expr) {
    Signed result;

    switch (expr.kind()) {
    case Kind::Integer:
        result.negated = expr.integer_value().is_negative();
        result.node = integer_node(result.negated ? -expr.integer_value()
                                                  : expr.integer_value());
        return result;

    case Kind::Real:
        // The sign bit, not `< 0`: -0.0 is not below zero but prints with a
        // minus. Found by fuzzing: kept inside the number, that minus escaped
        // the bracketing a negative base gets, and (-0.0)^-1 printed as
        // -0.0^(-1), which reads back as -(0.0^-1).
        result.negated = std::signbit(expr.real_value());
        result.node = display_node(DisplayReal{std::fabs(expr.real_value())});
        return result;

    case Kind::Rational: {
        result.negated = expr.numerator().is_negative();
        result.node = display_node(DisplayFraction(
            integer_node(result.negated ? -expr.numerator() : expr.numerator()),
            integer_node(expr.denominator())));
        return result;
    }

    case Kind::Symbol:
        result.node = display_node(DisplaySymbol{expr.name()});
        return result;

    case Kind::Opaque:
        result.node = display_node(DisplayVerbatim{expr.opaque_text()});
        return result;

    case Kind::Add:
        result.node = from_sum(expr.args());
        return result;

    case Kind::Mul:
        return from_product(expr.args());

    case Kind::Pow:
        result.node = from_power(expr.arg(0), expr.arg(1));
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
            result.node = display_node(DisplayList{std::move(args)});
            return result;
        }
        // Written `x!` and `x!!` by a renderer that has the notation, and
        // Expr::parse reads it: the infix printer uses it so that a factorial
        // prints in the shape it is parsed from.
        if ((expr.name() == "factorial" || expr.name() == "double_factorial")
            && args.size() == 1) {
            result.node
                = display_node(DisplayPostfix(expr.name(), std::move(args.front())));
            return result;
        }
        result.node = display_node(DisplayCall{expr.name(), std::move(args)});
        return result;
    }

    case Kind::Relation:
        result.node = display_node(DisplayRelation(
            expr.relation_op(), display(expr.arg(0)), display(expr.arg(1))));
        return result;
    }

    result.node = display_node(DisplayVerbatim{});
    return result;
}

} // namespace

DisplayNode to_display(const Expr &expr) {
    return display(expr);
}

} // namespace proxima::detail
