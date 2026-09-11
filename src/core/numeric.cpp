#include <mx/numeric.hpp>

#include <mx/errors.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <string_view>
#include <utility>

namespace mx {
namespace {

using detail::Instruction;

// --- what a function means ------------------------------------------------
//
// One table, consulted by both the one-shot evaluator and the compiler, so the
// two cannot end up disagreeing about what `log` is.
//
// Deliberately a closed set: an unknown head is an error rather than a guess.
// Silently returning something plausible for a function that is not actually
// implemented would be far worse than refusing.

struct Builtin {
    std::string_view name;
    int arity; ///< -1 for variadic, needing at least one argument.
    double (*apply)(const double *args, std::size_t count);
};

template <double (*Fn)(double)>
double unary(const double *args, std::size_t) {
    return Fn(args[0]);
}

template <double (*Fn)(double, double)>
double binary(const double *args, std::size_t) {
    return Fn(args[0], args[1]);
}

double signumOf(const double *args, std::size_t) {
    return static_cast<double>((args[0] > 0) - (args[0] < 0));
}

double maximumOf(const double *args, std::size_t count) {
    double result = args[0];
    for (std::size_t i = 1; i < count; ++i) {
        result = std::fmax(result, args[i]);
    }
    return result;
}

double minimumOf(const double *args, std::size_t count) {
    double result = args[0];
    for (std::size_t i = 1; i < count; ++i) {
        result = std::fmin(result, args[i]);
    }
    return result;
}

constexpr Builtin kBuiltins[] = {
    {"sin", 1, unary<std::sin>},       {"cos", 1, unary<std::cos>},
    {"tan", 1, unary<std::tan>},       {"asin", 1, unary<std::asin>},
    {"acos", 1, unary<std::acos>},     {"atan", 1, unary<std::atan>},
    {"sinh", 1, unary<std::sinh>},     {"cosh", 1, unary<std::cosh>},
    {"tanh", 1, unary<std::tanh>},     {"asinh", 1, unary<std::asinh>},
    {"acosh", 1, unary<std::acosh>},   {"atanh", 1, unary<std::atanh>},
    {"exp", 1, unary<std::exp>},       {"log", 1, unary<std::log>},
    {"sqrt", 1, unary<std::sqrt>},     {"abs", 1, unary<std::fabs>},
    {"floor", 1, unary<std::floor>},   {"ceiling", 1, unary<std::ceil>},
    {"round", 1, unary<std::round>},   {"erf", 1, unary<std::erf>},
    {"signum", 1, signumOf},           {"atan2", 2, binary<std::atan2>},
    {"mod", 2, binary<std::fmod>},     {"max", -1, maximumOf},
    {"min", -1, minimumOf},
};

/// Index into kBuiltins for a name applicable to `count` arguments.
std::optional<std::size_t> builtinFor(std::string_view name, std::size_t count) {
    for (std::size_t i = 0; i < std::size(kBuiltins); ++i) {
        const Builtin &builtin = kBuiltins[i];
        if (builtin.name != name) {
            continue;
        }
        const bool fits = builtin.arity < 0
                              ? count >= 1
                              : count == static_cast<std::size_t>(builtin.arity);
        if (fits) {
            return i;
        }
    }
    return std::nullopt;
}

/// Constants Maxima names, recognised so that a result containing %pi can be
/// evaluated without the caller having to bind it.
bool namedConstant(std::string_view name, double &value) {
    if (name == "%pi") {
        value = std::numbers::pi;
    } else if (name == "%e") {
        value = std::numbers::e;
    } else if (name == "%phi") {
        value = std::numbers::phi;
    } else if (name == "inf") {
        value = std::numeric_limits<double>::infinity();
    } else if (name == "minf") {
        value = -std::numeric_limits<double>::infinity();
    } else {
        return false;
    }
    return true;
}

/// b^e for a small integer e, by squaring. Called where std::pow would be,
/// which for e of 2 or -1 is an order of magnitude more work than it needs.
double integerPower(double base, std::int32_t exponent) {
    const bool invert = exponent < 0;
    auto remaining = static_cast<std::uint32_t>(invert ? -static_cast<std::int64_t>(exponent)
                                                       : exponent);
    double result = 1.0;
    while (remaining != 0) {
        if ((remaining & 1u) != 0) {
            result *= base;
        }
        base *= base;
        remaining >>= 1;
    }
    return invert ? 1.0 / result : result;
}

double numericValueOf(const Expr &expr) {
    switch (expr.kind()) {
    case Kind::Integer:
        return expr.integerValue().toDouble();
    case Kind::Rational:
        return expr.numerator().toDouble() / expr.denominator().toDouble();
    default:
        return expr.realValue();
    }
}

// --- the one-shot evaluator -----------------------------------------------
//
// Kept as a direct walk rather than being routed through Compiled: compiling
// costs a traversal and several allocations, which for a single evaluation
// would be more work than the evaluation itself.

bool walk(const Expr &expr, const Bindings &bindings, double &result,
          std::string *failure) {
    const auto fail = [&](std::string reason) {
        if (failure != nullptr) {
            *failure = std::move(reason);
        }
        return false;
    };

    switch (expr.kind()) {
    case Kind::Integer:
    case Kind::Rational:
    case Kind::Real:
        result = numericValueOf(expr);
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
            if (!walk(operand, bindings, value, failure)) {
                return false;
            }
            result = isSum ? result + value : result * value;
        }
        return true;
    }

    case Kind::Pow: {
        double base = 0.0;
        double exponent = 0.0;
        if (!walk(expr.arg(0), bindings, base, failure)
            || !walk(expr.arg(1), bindings, exponent, failure)) {
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
            if (!walk(operand, bindings, value, failure)) {
                return false;
            }
            args.push_back(value);
        }
        if (const auto builtin = builtinFor(expr.name(), args.size())) {
            result = kBuiltins[*builtin].apply(args.data(), args.size());
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
    if (!walk(expr, bindings, result, &failure)) {
        throw EvalError(failure);
    }
    return result;
}

bool isEvaluable(const Expr &expr, const Bindings &bindings) {
    double result = 0.0;
    std::string failure;
    return walk(expr, bindings, result, &failure);
}

// --- the compiled form ----------------------------------------------------

namespace {

/// Emits `expr` in postfix order, tracking how deep the working stack gets.
class Compiler {
public:
    Compiler(std::vector<Instruction> &code, std::vector<double> &constants,
             const std::vector<std::string> &variables, const Bindings &bound)
        : code_(code), constants_(constants), variables_(variables),
          bound_(bound) {}

    std::size_t maxDepth() const { return maxDepth_; }

    void emit(const Expr &expr) {
        switch (expr.kind()) {
        case Kind::Integer:
        case Kind::Rational:
        case Kind::Real:
            pushConstant(numericValueOf(expr));
            return;

        case Kind::Symbol:
            emitSymbol(expr);
            return;

        case Kind::Add:
        case Kind::Mul: {
            for (const Expr &operand : expr.args()) {
                emit(operand);
            }
            const auto count = static_cast<std::uint32_t>(expr.arity());
            append({expr.is(Kind::Add) ? Instruction::Op::Add
                                       : Instruction::Op::Multiply,
                    0, count},
                   count);
            return;
        }

        case Kind::Pow: {
            emit(expr.arg(0));
            // An integer exponent known now saves a std::pow on every single
            // evaluation. The bound keeps the squaring loop short; beyond it
            // std::pow is the better bet anyway.
            const Expr &exponent = expr.arg(1);
            const std::optional<std::int64_t> small
                = exponent.is(Kind::Integer) ? exponent.integerValue().toInt64()
                                             : std::nullopt;
            if (small && *small >= -64 && *small <= 64) {
                append({Instruction::Op::IntegerPower,
                        static_cast<std::uint32_t>(
                            static_cast<std::int32_t>(*small)),
                        1},
                       1);
                return;
            }
            emit(exponent);
            append({Instruction::Op::Power, 0, 2}, 2);
            return;
        }

        case Kind::Function: {
            for (const Expr &operand : expr.args()) {
                emit(operand);
            }
            const auto count = static_cast<std::uint32_t>(expr.arity());
            const auto builtin = builtinFor(expr.name(), expr.arity());
            if (!builtin) {
                throw EvalError("cannot evaluate " + expr.name()
                                + " numerically with " + std::to_string(count)
                                + " argument(s)");
            }
            append({Instruction::Op::Call, static_cast<std::uint32_t>(*builtin),
                    count},
                   count);
            return;
        }

        case Kind::Relation:
            throw EvalError("a relation has no numeric value: " + expr.str());

        case Kind::Opaque:
            throw EvalError("cannot evaluate the unmodelled expression "
                            + expr.str());
        }
        throw EvalError("cannot evaluate " + expr.str());
    }

private:
    /// Pops `consumed` values and pushes one, so the net effect is known.
    void append(Instruction instruction, std::uint32_t consumed) {
        code_.push_back(instruction);
        depth_ -= consumed;
        ++depth_;
        maxDepth_ = std::max(maxDepth_, depth_);
    }

    void pushConstant(double value) {
        // Folded here, so the same literal appearing twice costs one slot.
        const auto existing
            = std::find(constants_.begin(), constants_.end(), value);
        const auto index = existing != constants_.end()
                               ? static_cast<std::uint32_t>(
                                     existing - constants_.begin())
                               : static_cast<std::uint32_t>(constants_.size());
        if (existing == constants_.end()) {
            constants_.push_back(value);
        }
        append({Instruction::Op::Constant, index, 0}, 0);
    }

    void emitSymbol(const Expr &expr) {
        // A variable shadows a binding, which shadows a named constant.
        const auto variable
            = std::find(variables_.begin(), variables_.end(), expr.name());
        if (variable != variables_.end()) {
            append({Instruction::Op::Variable,
                    static_cast<std::uint32_t>(variable - variables_.begin()), 0},
                   0);
            return;
        }
        if (const auto found = bound_.find(expr.name()); found != bound_.end()) {
            pushConstant(found->second);
            return;
        }
        double value = 0.0;
        if (namedConstant(expr.name(), value)) {
            pushConstant(value);
            return;
        }
        throw EvalError("no value for the symbol " + expr.name());
    }

    std::vector<Instruction> &code_;
    std::vector<double> &constants_;
    const std::vector<std::string> &variables_;
    const Bindings &bound_;
    std::size_t depth_ = 0;
    std::size_t maxDepth_ = 0;
};

} // namespace

Compiled::Compiled(const Expr &expr, std::span<const Symbol> variables,
                   const Bindings &constants) {
    variables_.reserve(variables.size());
    for (const Symbol &variable : variables) {
        variables_.push_back(variable.name());
    }

    Compiler compiler(code_, constants_, variables_, constants);
    compiler.emit(expr);
    depth_ = compiler.maxDepth();
}

Compiled::Compiled(const Expr &expr, const Symbol &variable,
                   const Bindings &constants)
    : Compiled(expr, std::span<const Symbol>(&variable, 1), constants) {}

double Compiled::operator()(std::span<const double> values) const {
    if (values.size() != variables_.size()) {
        throw EvalError("expected " + std::to_string(variables_.size())
                        + " value(s) but was given "
                        + std::to_string(values.size()));
    }

    // Thread-local, so one Compiled can be shared across threads without
    // synchronisation and without allocating on every call.
    thread_local std::vector<double> scratch;
    if (scratch.size() < depth_) {
        scratch.resize(depth_);
    }
    double *stack = scratch.data();
    std::size_t top = 0;

    for (const Instruction &instruction : code_) {
        switch (instruction.op) {
        case Instruction::Op::Constant:
            stack[top++] = constants_[instruction.index];
            break;

        case Instruction::Op::Variable:
            stack[top++] = values[instruction.index];
            break;

        case Instruction::Op::Add: {
            top -= instruction.count;
            double sum = 0.0;
            for (std::uint32_t i = 0; i < instruction.count; ++i) {
                sum += stack[top + i];
            }
            stack[top++] = sum;
            break;
        }

        case Instruction::Op::Multiply: {
            top -= instruction.count;
            double product = 1.0;
            for (std::uint32_t i = 0; i < instruction.count; ++i) {
                product *= stack[top + i];
            }
            stack[top++] = product;
            break;
        }

        case Instruction::Op::Power:
            top -= 2;
            stack[top] = std::pow(stack[top], stack[top + 1]);
            ++top;
            break;

        case Instruction::Op::IntegerPower:
            stack[top - 1] = integerPower(
                stack[top - 1], static_cast<std::int32_t>(instruction.index));
            break;

        case Instruction::Op::Call:
            top -= instruction.count;
            stack[top] = kBuiltins[instruction.index].apply(stack + top,
                                                            instruction.count);
            ++top;
            break;
        }
    }
    return stack[0];
}

double Compiled::operator()(double value) const {
    return (*this)(std::span<const double>(&value, 1));
}

std::function<double(double)> asFunction(const Expr &expr, const Symbol &variable,
                                         Bindings fixed) {
    return [compiled = Compiled(expr, variable, fixed)](double value) {
        return compiled(value);
    };
}

} // namespace mx
