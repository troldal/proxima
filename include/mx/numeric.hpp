#pragma once

#include <mx/expr.hpp>
#include <mx/symbol.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace mx {

/// Values for the symbols in an expression, by name.
///
/// Keyed on the name rather than on Symbol so that a lookup can be made from a
/// string_view without building a Symbol, and so that the common case reads as
/// `{{"x", 2.0}, {"y", 3.0}}`.
using Bindings = std::map<std::string, double, std::less<>>;

/// Evaluates an expression to a double, once.
///
/// Entirely local: no kernel, no round trip. That is the point — once Maxima
/// has produced a closed form, turning it into numbers is ordinary arithmetic,
/// and paying milliseconds per point to ask Maxima would make plotting or
/// integrating it numerically absurd.
///
/// For repeated evaluation use mx::Compiled, which is far faster: this looks
/// every symbol up by name, in a map, on every occurrence.
///
/// Named constants are recognised as Maxima spells them: `%pi`, `%e`, `%phi`
/// (the golden ratio), `%gamma` (the Euler–Mascheroni constant), `inf` and
/// `minf`. An explicit binding wins over them, so a symbol called `%e` can be
/// given a different value if that is genuinely what is wanted.
///
/// Throws mx::EvalError for anything it cannot turn into a number: an unbound
/// symbol, a function it does not know, a relation, or an Opaque node — the
/// last being Maxima source text this library never interpreted, which is
/// precisely why it cannot be evaluated here.
double evalNumeric(const Expr &expr, const Bindings &bindings = {});

/// True when evalNumeric could succeed: every symbol bound and every function
/// known. Cheaper than catching, when the caller wants to ask before committing.
bool isEvaluable(const Expr &expr, const Bindings &bindings = {});

namespace detail {

/// One step of a compiled expression, in postfix order.
///
/// In the public header because Compiled stores these inline: hiding them
/// behind a pimpl would put an indirection in the middle of the loop this type
/// exists to make fast.
struct Instruction {
    enum class Op : std::uint8_t {
        Constant, ///< Push constants[index].
        Variable, ///< Push the caller's values[index].
        Add,      ///< Replace the top `count` values with their sum.
        Multiply, ///< Replace the top `count` values with their product.
        Power,    ///< Replace the top two with base^exponent.
        /// Raise the top value to a small integer power held in `index`,
        /// reinterpreted as signed. Worth its own opcode because std::pow is
        /// far more expensive than the handful of multiplications it replaces,
        /// and an exponent known at compile time is extremely common — `x^2`,
        /// and every division, which is a power of -1.
        IntegerPower,
        Call,     ///< Apply builtin `index` to the top `count` values.
    };

    Op op = Op::Constant;
    std::uint32_t index = 0;
    std::uint32_t count = 0;
};

} // namespace detail

/// An expression prepared once for evaluation many times.
///
/// The work `evalNumeric` repeats per call — walking the tree, looking each
/// symbol up by name in a map, resolving each function name — happens here
/// once. What is left is a flat instruction list over a small stack, with
/// symbols resolved to positions in the caller's argument array.
///
/// Errors surface at construction rather than on every call, which is where
/// they belong: an unknown function or an unbound symbol is a property of the
/// expression, not of the point being evaluated.
///
/// Evaluation is thread-safe: the working stack is thread-local, so one
/// Compiled can be shared by several threads without synchronisation.
class Compiled {
public:
    /// Prepares `expr` as a function of `variables`, in that order.
    ///
    /// Any other symbol must have a value in `constants` or be one of the named
    /// constants (`%pi`, `%e`, `%phi`, `%gamma`, `inf`, `minf`), and is folded
    /// in now. A name in
    /// `variables` shadows both.
    ///
    /// Throws mx::EvalError if the expression cannot be turned into numbers.
    Compiled(const Expr &expr, std::span<const Symbol> variables,
             const Bindings &constants = {});

    /// Single-variable form, for the common case.
    Compiled(const Expr &expr, const Symbol &variable,
             const Bindings &constants = {});

    /// Evaluates at `values`, which must have exactly arity() entries.
    double operator()(std::span<const double> values) const;

    /// Evaluates a single-variable expression.
    double operator()(double value) const;

    std::size_t arity() const { return variables_.size(); }
    const std::vector<std::string> &variableNames() const { return variables_; }

    /// Number of instructions, a rough measure of the expression's size.
    std::size_t size() const { return code_.size(); }

private:
    std::vector<detail::Instruction> code_;
    std::vector<double> constants_;
    std::vector<std::string> variables_;
    std::size_t depth_ = 0;
};

/// Binds `expr` to one variable for repeated evaluation.
///
/// Backed by a Compiled, so the returned function is cheap to call.
std::function<double(double)> asFunction(const Expr &expr, const Symbol &variable,
                                         Bindings fixed = {});

} // namespace mx
