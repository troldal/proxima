#include <mx/expr.hpp>

#include <charconv>
#include <string>

// Written entirely against Expr's public accessors rather than Node, so that
// the printer doubles as a check that the public API is sufficient to read a
// tree. The output is Maxima-compatible infix, which is also what the
// Expr -> Maxima direction of the translation layer will need (PLAN.md step 9).

namespace mx {
namespace {

// Binding strength, used to decide parentheses. A child is wrapped when it
// binds more loosely than its position allows.
enum Precedence {
    kLoosest = 0,
    kRelation = 10,
    kAdd = 20,
    kMul = 30,
    kPow = 40,
    kAtom = 50,
};

int precedenceOf(const Expr &expr) {
    switch (expr.kind()) {
    case Kind::Relation:
        return kRelation;
    case Kind::Add:
        return kAdd;
    case Kind::Mul:
        return kMul;
    case Kind::Pow:
        return kPow;
    case Kind::Rational:
        // A fraction is a division, so it binds like a product.
        return expr.isNegativeNumber() ? kAdd : kMul;
    case Kind::Integer:
    case Kind::Real:
        // A negative literal carries a sign that would bind wrongly inside a
        // product or a power: (-3)^2, not -3^2.
        return expr.isNegativeNumber() ? kAdd : kAtom;
    default:
        return kAtom;
    }
}

std::string renderReal(double value) {
    char buffer[40];
    const auto [stopped, error]
        = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (error != std::errc{}) {
        return "0.0";
    }
    std::string text(buffer, stopped);
    // Keep it recognisable as inexact rather than reading back as an integer.
    if (text.find_first_of(".eni") == std::string::npos) {
        text += ".0";
    }
    return text;
}

std::string render(const Expr &expr, int context);

std::string wrap(const Expr &expr, int context) {
    std::string text = render(expr, context);
    if (precedenceOf(expr) < context) {
        return "(" + text + ")";
    }
    return text;
}

/// Splits a leading minus sign off a term so a sum can print `a - b` instead of
/// `a + -b`. Returns false when the term is not negative-looking.
bool negativeTerm(const Expr &term, std::string &rendered) {
    if (term.isNegativeNumber()) {
        rendered = render(-term, kAdd);
        return true;
    }
    // A product whose first factor is a negative number: -1*x, -3*sin(x).
    if (term.is(Kind::Mul) && term.arity() >= 2
        && term.arg(0).isNegativeNumber()) {
        std::vector<Expr> factors(term.args().begin() + 1, term.args().end());
        const Expr positive = -term.arg(0);

        // -1*x reads better as just x, with the sign carried by the operator.
        if (positive.is(Kind::Integer) && positive.integerValue() == 1) {
            rendered = wrap(Expr::mul(std::move(factors)), kAdd);
        } else {
            factors.insert(factors.begin(), positive);
            rendered = wrap(Expr::mul(std::move(factors)), kAdd);
        }
        return true;
    }
    return false;
}

std::string render(const Expr &expr, int context) {
    switch (expr.kind()) {
    case Kind::Integer:
        return std::to_string(expr.integerValue());

    case Kind::Rational:
        return std::to_string(expr.numerator()) + "/"
               + std::to_string(expr.denominator());

    case Kind::Real:
        return renderReal(expr.realValue());

    case Kind::Symbol:
        return expr.name();

    case Kind::Opaque:
        // Already Maxima source; reproduced verbatim.
        return expr.opaqueText();

    case Kind::Add: {
        std::string out;
        for (std::size_t i = 0; i < expr.arity(); ++i) {
            const Expr &term = expr.arg(i);
            std::string negated;
            if (i != 0 && negativeTerm(term, negated)) {
                out += " - ";
                out += negated;
                continue;
            }
            if (i != 0) {
                out += " + ";
            }
            out += wrap(term, kAdd);
        }
        return out;
    }

    case Kind::Mul: {
        std::string out;
        for (std::size_t i = 0; i < expr.arity(); ++i) {
            if (i != 0) {
                out += "*";
            }
            const Expr &factor = expr.arg(i);
            // A *leading* negative literal needs no parentheses: Maxima reads
            // -2*x as -(2*x), which is the same value. A later one does need
            // them, because `x*-2` is not valid Maxima at all. And a negative
            // base of a power always does — -3^2 is -9, not 9.
            if (i == 0 && factor.isNegativeNumber()) {
                out += render(factor, kLoosest);
            } else {
                out += wrap(factor, kMul);
            }
        }
        return out;
    }

    case Kind::Pow:
        // '^' is right-associative in Maxima, so the exponent needs no
        // parentheses for nesting but the base does.
        return wrap(expr.arg(0), kPow + 1) + "^" + wrap(expr.arg(1), kPow);

    case Kind::Function: {
        std::string out = expr.name();
        out += "(";
        for (std::size_t i = 0; i < expr.arity(); ++i) {
            if (i != 0) {
                out += ", ";
            }
            out += render(expr.arg(i), kLoosest);
        }
        out += ")";
        return out;
    }

    case Kind::Relation:
        return wrap(expr.arg(0), kRelation + 1) + " "
               + std::string(symbolFor(expr.relationOp())) + " "
               + wrap(expr.arg(1), kRelation + 1);
    }

    static_cast<void>(context);
    return {};
}

} // namespace

std::string Expr::str() const {
    return render(*this, kLoosest);
}

} // namespace mx
