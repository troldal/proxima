#include <mx/context.hpp>

#include "wire/from_maxima.hpp"
#include "wire/sexpr.hpp"

#include <mx/errors.hpp>

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
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

/// What this library knows about one Maxima context it opened.
struct Scope {
    std::string parent;
    Kernel *kernel = nullptr;

    /// Scopes opened inside this one that have not been torn down. While any
    /// remain, this one stays in Maxima even after its Context has ended:
    /// they were opened inheriting its facts.
    std::size_t openChildren = 0;

    /// True once the Context object has been destroyed.
    bool ended = false;

    /// The ended Context's journal handles, kept until the teardown actually
    /// happens, so a restart in the meantime still rebuilds this scope — the
    /// scopes opened inside it are subcontexts of it and cannot be rebuilt
    /// without it.
    std::vector<std::uint64_t> replayHandles;
};

/// Every context this library has opened and not yet torn down, by name.
///
/// Maxima can list one context's facts, but not a context's ancestry or its
/// subcontexts, so this is what lets facts() walk outward and lets a scope that
/// ends before its inner scopes wait for them. Process-wide, like the names it
/// is keyed by, and locked, because separate kernels may be used from separate
/// threads.
struct Registry {
    std::mutex mutex;
    std::unordered_map<std::string, Scope> scopes;
};

Registry &registry() {
    static Registry instance;
    return instance;
}

/// Maxima's root context: the built-in facts of its type system, such as
/// `kind(%e, irrational)`, seventy-odd of them. Not anything a caller assumed.
constexpr std::string_view kBuiltInContext = "global";

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

/// One scope to remove from Maxima, gathered under the registry lock and
/// carried out after it is released, so no lock is held across a round trip.
struct Teardown {
    Kernel *kernel;
    std::string name;
    std::string parent;
    std::vector<std::uint64_t> replayHandles;
};

void carryOut(const Teardown &step) {
    // Drop the replay entries first: a restart triggered by the teardown
    // itself must not rebuild a scope that is ending.
    for (auto handle = step.replayHandles.rbegin();
         handle != step.replayHandles.rend(); ++handle) {
        step.kernel->forget(*handle);
    }
    // An assignment has no Expr, so this one line stays as text. Both names are
    // this library's own or Maxima's, never the user's.
    step.kernel->evalTracked("context: " + step.parent);
    step.kernel->evalTracked(call("killcontext", {Expr::symbol(step.name)}));
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

    const std::lock_guard lock(registry().mutex);
    Scope &scope = registry().scopes[name_];
    scope.parent = parent_;
    scope.kernel = kernel_;
    if (const auto enclosing = registry().scopes.find(parent_);
        enclosing != registry().scopes.end()) {
        ++enclosing->second.openChildren;
    }
}

Context::~Context() {
    // A destructor that throws during stack unwinding would terminate the
    // process, and failing to tidy up a context is not worth that. The next
    // kernel restart clears it regardless.
    try {
        // Scopes need not end innermost first: a Context on the heap, or held
        // by another object, can outlive the one it was opened inside. Maxima
        // has no answer for that — killing a context leaves its subcontexts
        // orphaned, without the facts they inherited — and switching Maxima to
        // this scope's parent would deactivate a scope that is still open, own
        // facts and all. So a scope with open inner scopes only marks itself
        // ended, and is torn down when the last of them is.
        std::vector<Teardown> steps;
        {
            const std::lock_guard lock(registry().mutex);
            auto &scopes = registry().scopes;
            const auto self = scopes.find(name_);
            if (self == scopes.end()) {
                steps.push_back({kernel_, name_, parent_, replayHandles_});
            } else {
                self->second.ended = true;
                self->second.replayHandles = replayHandles_;

                // Innermost first: this scope, then each enclosing scope that
                // had already ended and was only waiting for this one.
                std::string next = name_;
                for (;;) {
                    const auto found = scopes.find(next);
                    if (found == scopes.end() || !found->second.ended
                        || found->second.openChildren > 0) {
                        break;
                    }
                    Scope scope = std::move(found->second);
                    scopes.erase(found);
                    steps.push_back({scope.kernel, next, scope.parent,
                                     std::move(scope.replayHandles)});

                    const auto enclosing = scopes.find(scope.parent);
                    if (enclosing == scopes.end()) {
                        break;
                    }
                    --enclosing->second.openChildren;
                    next = scope.parent;
                }
            }
        }
        for (const Teardown &step : steps) {
            carryOut(step);
        }
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
    // Maxima's facts(name) lists the facts of that one context, not its
    // ancestors'; and a bare facts() lists whichever context happens to be
    // current, which is not necessarily this one. This used to be a bare
    // facts(), so an inner scope reported only its own declarations, and an
    // outer scope asked while an inner one was open reported the inner one's.
    std::vector<std::string> chain{name_};
    {
        const std::lock_guard lock(registry().mutex);
        std::string next = parent_;
        while (next != kBuiltInContext) {
            chain.push_back(next);
            // An enclosing scope whose Context has ended but which is still
            // waiting for this one counts: its facts are still in force here.
            const auto found = registry().scopes.find(next);
            if (found == registry().scopes.end()) {
                // Not one of ours — normally `initial`, where facts assumed
                // outside any Context live. Its parent is `global`.
                break;
            }
            next = found->second.parent;
        }
    }

    std::vector<Expr> all;
    for (const std::string &name : chain) {
        const Expr result
            = evaluateOrThrow(*kernel_, call("facts", {Expr::symbol(name)}));
        if (result.is(Kind::Function) && result.name() == "list") {
            all.insert(all.end(), result.args().begin(), result.args().end());
        }
    }
    return all;
}

} // namespace mx
