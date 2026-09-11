#include <mx/context.hpp>

#include "wire/from_maxima.hpp"
#include "wire/sexpr.hpp"

#include <mx/errors.hpp>

#include <atomic>
#include <string>
#include <utility>
#include <vector>

namespace mx {
namespace {

/// Names have to be unique within the Maxima process, and a Context may be
/// created in any order or nesting, so they are simply counted.
std::string nextContextName() {
    static std::atomic<unsigned long long> counter{0};
    return "mx_ctx_" + std::to_string(++counter);
}

/// Every statement a Context issues changes Maxima's state in a way the
/// kernel's journal accounts for — either recorded here, or undoing something
/// that was. evalTracked says so, which is what keeps a persistent cache usable
/// for a kernel that uses assumptions.
///
/// Sent as a form, not text: the predicate is the user's, and travelling as
/// structure is what guarantees it cannot be misread on the far side.
Expr evaluateOrThrow(Kernel &kernel, const Expr &form) {
    const Reply reply = kernel.evalTracked(form);
    if (!reply.ok) {
        throw MaximaError(reply.reason);
    }
    return detail::fromMaxima(detail::parseSExpr(reply.value));
}

Expr call(std::string head, std::vector<Expr> args) {
    return Expr::function(std::move(head), std::move(args));
}

/// True when Maxima's reply contains the symbol `name` — how it reports
/// `redundant` and `inconsistent` from an assume.
bool mentionsSymbol(const Expr &expr, std::string_view name) {
    if (expr.is(Kind::Symbol)) {
        return expr.name() == name;
    }
    for (const Expr &operand : expr.args()) {
        if (mentionsSymbol(operand, name)) {
            return true;
        }
    }
    return false;
}

} // namespace

std::string_view nameOf(Feature feature) {
    switch (feature) {
    case Feature::Integer:
        return "integer";
    case Feature::NonInteger:
        return "noninteger";
    case Feature::Even:
        return "even";
    case Feature::Odd:
        return "odd";
    case Feature::Rational:
        return "rational";
    case Feature::Irrational:
        return "irrational";
    case Feature::Real:
        return "real";
    case Feature::Imaginary:
        return "imaginary";
    case Feature::Complex:
        return "complex";
    case Feature::Constant:
        return "constant";
    case Feature::Prime:
        return "prime";
    case Feature::Increasing:
        return "increasing";
    case Feature::Decreasing:
        return "decreasing";
    }
    return "real";
}

Context::Context(Kernel &kernel) : kernel_(&kernel), name_(nextContextName()) {
    // Whatever is active now becomes this context's parent, which is what makes
    // nesting inherit rather than shadow.
    const Expr current = evaluateOrThrow(*kernel_, Expr::symbol("context"));
    parent_ = current.is(Kind::Symbol) ? current.name() : "initial";

    // supcontext rather than newcontext: newcontext would parent the new
    // context on `initial` and so lose the enclosing scope's assumptions.
    const Expr create
        = call("supcontext", {Expr::symbol(name_), Expr::symbol(parent_)});
    evaluateOrThrow(*kernel_, create);
    replayHandles_.push_back(kernel_->remember(create));
}

Context::~Context() {
    // A destructor that throws during stack unwinding would terminate the
    // process, and failing to tidy up a context is not worth that. The next
    // kernel restart clears it regardless.
    try {
        // Drop the replay entries first: a restart triggered by the teardown
        // itself must not rebuild a scope that is ending.
        for (auto handle = replayHandles_.rbegin();
             handle != replayHandles_.rend(); ++handle) {
            kernel_->forget(*handle);
        }
        // An assignment has no Expr, so this one line stays as text. Both
        // names are this library's own or Maxima's, never the user's.
        kernel_->evalTracked("context: " + parent_);
        kernel_->evalTracked(call("killcontext", {Expr::symbol(name_)}));
    } catch (...) {
    }
}

void Context::assume(const Expr &predicate) {
    const Expr statement = call("assume", {predicate});
    const Expr result = evaluateOrThrow(*kernel_, statement);

    // Maxima answers with a list describing what it did. `inconsistent` means
    // this contradicts something already in force; carrying on would make every
    // later result in this scope meaningless.
    if (mentionsSymbol(result, "inconsistent")) {
        throw MaximaError("the assumption " + predicate.str()
                          + " contradicts one already in force");
    }
    // `redundant` is harmless: the fact was already implied.
    assumptions_.push_back(predicate);
    replayHandles_.push_back(kernel_->remember(statement));
}

void Context::declare(const Symbol &symbol, Feature feature) {
    const Expr statement
        = call("declare", {symbol, Expr::symbol(std::string(nameOf(feature)))});
    evaluateOrThrow(*kernel_, statement);
    replayHandles_.push_back(kernel_->remember(statement));
}

std::vector<Expr> Context::facts() const {
    const Expr result = evaluateOrThrow(*kernel_, call("facts", {}));
    if (result.is(Kind::Function) && result.name() == "list") {
        return {result.args().begin(), result.args().end()};
    }
    return {};
}

} // namespace mx
