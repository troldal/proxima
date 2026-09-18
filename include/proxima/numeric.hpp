#pragma once

#include <proxima/expr.hpp>
#include <proxima/result.hpp>
#include <proxima/symbol.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace proxima {

/// Values for the symbols in an expression.
///
/// A symbol can be named either way — by the Symbol an expression was built
/// with, or by its name — so `{{x, 2.0}, {"y", 3.0}}` reads as consistently as
/// Compiled's list of variables does. Stored by name either way, so that the
/// lookup eval_numeric makes for every symbol it meets builds nothing.
///
/// A name given twice keeps the last value given.
class Bindings {
public:
    /// One value, for a symbol or a name.
    struct Entry {
        Entry(const Symbol &symbol, double bound_value)
            : name(symbol.name()), value(bound_value) {}
        Entry(std::string_view symbol_name, double bound_value)
            : name(symbol_name), value(bound_value) {}

        std::string name;
        double value;
    };

    using const_iterator
        = std::map<std::string, double, std::less<>>::const_iterator;

    Bindings() = default;

    Bindings(
        std::initializer_list<Entry> entries) { // NOLINT: `{{x, 1.0}}` is the point.
        for (const Entry &entry : entries) {
            values_.insert_or_assign(entry.name, entry.value);
        }
    }

    /// These bindings with one more — or one replaced — leaving these as they
    /// are. The value-oriented way to build bindings up, and the one to use
    /// on bindings shared between callers:
    ///
    ///     const Bindings base{{"g", 9.81}};
    ///     const Bindings here = base.with("h", 2.0);   // base still has only g
    [[nodiscard]] Bindings with(const Symbol &symbol, double value) const {
        return with(symbol.name(), value);
    }
    [[nodiscard]] Bindings with(std::string_view symbol_name, double value) const {
        Bindings copy = *this;
        copy.set(symbol_name, value);
        return copy;
    }

    /// Gives a symbol a value in place, replacing any it had. For bindings
    /// being built up locally; with() leaves the original alone.
    void set(const Symbol &symbol, double value) { set(symbol.name(), value); }
    void set(std::string_view symbol_name, double value) {
        values_.insert_or_assign(std::string(symbol_name), value);
    }

    /// The entry for a symbol, or end().
    const_iterator find(std::string_view symbol_name) const {
        return values_.find(symbol_name);
    }
    const_iterator find(const Symbol &symbol) const { return find(symbol.name()); }

    bool contains(std::string_view symbol_name) const {
        return values_.contains(symbol_name);
    }
    bool contains(const Symbol &symbol) const { return contains(symbol.name()); }

    /// Name–value pairs, in name order.
    const_iterator begin() const { return values_.begin(); }
    const_iterator end() const { return values_.end(); }
    std::size_t size() const { return values_.size(); }
    bool empty() const { return values_.empty(); }

private:
    std::map<std::string, double, std::less<>> values_;
};

/// Evaluates an expression to a double, once.
///
/// Entirely local: no kernel, no round trip. That is the point — once Maxima
/// has produced a closed form, turning it into numbers is ordinary arithmetic,
/// and paying milliseconds per point to ask Maxima would make plotting or
/// integrating it numerically absurd.
///
/// For repeated evaluation use proxima::Compiled, which is far faster: this looks
/// every symbol up by name, in a map, on every occurrence.
///
/// Named constants are recognised as Maxima spells them: `%pi`, `%e`, `%phi`
/// (the golden ratio), `%gamma` (the Euler–Mascheroni constant), `inf` and
/// `minf`. An explicit binding wins over them, so a symbol called `%e` can be
/// given a different value if that is genuinely what is wanted.
///
/// A Failure, with Cause::Eval, for anything it cannot turn into a number: an
/// unbound symbol, a function it does not know, a relation, or an Opaque
/// node — the last being Maxima source text this library never interpreted,
/// which is precisely why it cannot be evaluated here.
result<double> eval_numeric(const Expr &expr, const Bindings &bindings = {});

/// True when eval_numeric could succeed: every symbol bound and every function
/// known. Cheaper than evaluating, when the caller wants to ask before
/// committing, and builds no message.
bool is_evaluable(const Expr &expr, const Bindings &bindings = {});

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
        Call, ///< Apply builtin `index` to the top `count` values.
    };

    Op op = Op::Constant;
    std::uint32_t index = 0;
    std::uint32_t count = 0;
};

} // namespace detail

/// An expression prepared once for evaluation many times.
///
/// The work `eval_numeric` repeats per call — walking the tree, looking each
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
    /// Throws proxima::EvalError if the expression cannot be turned into
    /// numbers. A constructor cannot return a Failure; proxima::compile can,
    /// and is the same preparation as a value.
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
    const std::vector<std::string> &variable_names() const { return variables_; }

    /// Number of instructions, a rough measure of the expression's size.
    std::size_t size() const { return code_.size(); }

private:
    std::vector<detail::Instruction> code_;
    std::vector<double> constants_;
    std::vector<std::string> variables_;
    std::size_t depth_ = 0;
};

/// Prepares `expr` as a function of `variables`, as Compiled's constructor
/// does, with a Failure (Cause::Eval) in place of its exception.
result<Compiled> compile(const Expr &expr, std::span<const Symbol> variables,
                         const Bindings &constants = {});
result<Compiled> compile(const Expr &expr, const Symbol &variable,
                         const Bindings &constants = {});

/// Binds `expr` to one variable for repeated evaluation: the Compiled for it,
/// which is itself a callable `double(double)`.
///
/// Returned as the Compiled rather than wrapped in a std::function, which cost
/// an allocation and an indirect call per point for nothing: a Compiled
/// converts to a std::function<double(double)> wherever one is wanted.
/// Throws proxima::EvalError, as Compiled's constructor does; compile() is the
/// same preparation as a result.
Compiled as_function(const Expr &expr, const Symbol &variable,
                     const Bindings &fixed = {});

} // namespace proxima
