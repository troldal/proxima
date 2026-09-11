#include <mx/numeric.hpp>

#include <mx/errors.hpp>

#include <cmath>
#include <limits>
#include <numbers>
#include <string_view>
#include <utility>

namespace mx {
namespace {

/// Constants Maxima names, recognised here so that a result containing %pi is
/// evaluable without the caller having to bind it.
bool namedConstant(std::string_view name, double &value) {
    if (name == "%pi") {
        value = std::numbers::pi;
        return true;
    }
    if (name == "%e") {
        value = std::numbers::e;
        return true;
    }
    if (name == "%phi") {
        value = std::numbers::phi;
        return true;
    }
    if (name == "inf") {
        value = std::numeric_limits<double>::infinity();
        return true;
    }
    if (name == "minf") {
        value = -std::numeric_limits<double>::infinity();
        return true;
    }
    return false;
}

/// Applies a function Maxima might have left in the result.
///
/// Deliberately a closed set: an unknown head is an error rather than a guess.
/// Silently returning something plausible for a function this does not actually
/// implement would be far worse than refusing.
bool applyFunction(std::string_view name, const std::vector<double> &args,
                   double &result) {
    const auto unary = [&](double (*fn)(double)) {
        if (args.size() != 1) {
            return false;
        }
        result = fn(args[0]);
        return true;
    };
    const auto binary = [&](double (*fn)(double, double)) {
        if (args.size() != 2) {
            return false;
        }
        result = fn(args[0], args[1]);
        return true;
    };

    if (name == "sin") return unary(std::sin);
    if (name == "cos") return unary(std::cos);
    if (name == "tan") return unary(std::tan);
    if (name == "asin") return unary(std::asin);
    if (name == "acos") return unary(std::acos);
    if (name == "atan") return unary(std::atan);
    if (name == "sinh") return unary(std::sinh);
    if (name == "cosh") return unary(std::cosh);
    if (name == "tanh") return unary(std::tanh);
    if (name == "asinh") return unary(std::asinh);
    if (name == "acosh") return unary(std::acosh);
    if (name == "atanh") return unary(std::atanh);
    if (name == "exp") return unary(std::exp);
    if (name == "log") return unary(std::log); // Maxima's log is natural.
    if (name == "sqrt") return unary(std::sqrt);
    if (name == "abs") return unary(std::fabs);
    if (name == "floor") return unary(std::floor);
    if (name == "ceiling") return unary(std::ceil);
    if (name == "round") return unary(std::round);
    if (name == "erf") return unary(std::erf);

    if (name == "atan2") return binary(std::atan2);
    if (name == "mod") return binary(std::fmod);

    if (name == "signum") {
        if (args.size() != 1) {
            return false;
        }
        result = (args[0] > 0) - (args[0] < 0);
        return true;
    }
    if (name == "max" || name == "min") {
        if (args.empty()) {
            return false;
        }
        result = args.front();
        for (const double value : args) {
            result = name == "max" ? std::fmax(result, value)
                                   : std::fmin(result, value);
        }
        return true;
    }
    return false;
}

/// Walks the tree. `failure`, when non-null, receives the reason instead of an
/// exception being thrown, which is how isEvaluable asks without catching.
bool evaluate(const Expr &expr, const Bindings &bindings, double &result,
              std::string *failure) {
    const auto fail = [&](std::string reason) {
        if (failure != nullptr) {
            *failure = std::move(reason);
        }
        return false;
    };

    switch (expr.kind()) {
    case Kind::Integer:
        result = static_cast<double>(expr.integerValue());
        return true;

    case Kind::Rational:
        result = static_cast<double>(expr.numerator())
                 / static_cast<double>(expr.denominator());
        return true;

    case Kind::Real:
        result = expr.realValue();
        return true;

    case Kind::Symbol: {
        // An explicit binding wins over a named constant, so a symbol that
        // happens to be called %e can still be given a value.
        if (const auto bound = bindings.find(expr.name()); bound != bindings.end()) {
            result = bound->second;
            return true;
        }
        if (namedConstant(expr.name(), result)) {
            return true;
        }
        return fail("no value for the symbol " + expr.name());
    }

    case Kind::Add:
    case Kind::Mul: {
        const bool isSum = expr.is(Kind::Add);
        result = isSum ? 0.0 : 1.0;
        for (const Expr &operand : expr.args()) {
            double value = 0.0;
            if (!evaluate(operand, bindings, value, failure)) {
                return false;
            }
            result = isSum ? result + value : result * value;
        }
        return true;
    }

    case Kind::Pow: {
        double base = 0.0;
        double exponent = 0.0;
        if (!evaluate(expr.arg(0), bindings, base, failure)
            || !evaluate(expr.arg(1), bindings, exponent, failure)) {
            return false;
        }
        result = std::pow(base, exponent);
        return true;
    }

    case Kind::Function: {
        std::vector<double> args;
        args.reserve(expr.arity());
        for (const Expr &operand : expr.args()) {
            double value = 0.0;
            if (!evaluate(operand, bindings, value, failure)) {
                return false;
            }
            args.push_back(value);
        }
        if (applyFunction(expr.name(), args, result)) {
            return true;
        }
        return fail("cannot evaluate " + expr.name() + " numerically with "
                    + std::to_string(args.size()) + " argument(s)");
    }

    case Kind::Relation:
        return fail("a relation has no numeric value: " + expr.str());

    case Kind::Opaque:
        // Maxima source this library never interpreted, which is exactly why
        // there is nothing here that could evaluate it.
        return fail("cannot evaluate the unmodelled expression " + expr.str());
    }
    return fail("cannot evaluate " + expr.str());
}

} // namespace

double evalNumeric(const Expr &expr, const Bindings &bindings) {
    double result = 0.0;
    std::string failure;
    if (!evaluate(expr, bindings, result, &failure)) {
        throw EvalError(failure);
    }
    return result;
}

bool isEvaluable(const Expr &expr, const Bindings &bindings) {
    double result = 0.0;
    std::string failure;
    return evaluate(expr, bindings, result, &failure);
}

std::function<double(double)> asFunction(const Expr &expr, const Symbol &variable,
                                         Bindings fixed) {
    return [expr, name = variable.name(), fixed = std::move(fixed)](double value) {
        Bindings bindings = fixed;
        bindings[name] = value;
        return evalNumeric(expr, bindings);
    };
}

} // namespace mx
