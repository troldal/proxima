#include <proxima/context.hpp>

#include "kernel/session.hpp"
#include "wire/from_maxima.hpp"
#include "wire/sexpr.hpp"
#include "wire/to_maxima.hpp"

#include <proxima/errors.hpp>
#include <proxima/traverse.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace proxima {
namespace {

using Conversation = detail::MaximaSession::Conversation;

/// Names have to be unique within the Maxima process, and a Context may be
/// created in any order or nesting, so they are simply counted.
std::string next_context_name() {
    static std::atomic<unsigned long long> counter{0};
    return "proxima_ctx_" + std::to_string(++counter);
}

/// What this library knows about one Maxima context it opened.
struct Scope {
    std::string parent;
    Kernel *kernel = nullptr;

    /// Expired once the kernel is gone, when there is no Maxima left to tidy.
    std::weak_ptr<const int> kernel_lifetime;

    /// Scopes opened inside this one that have not been torn down. While any
    /// remain, this one stays in Maxima even after its Context has ended:
    /// they were opened inheriting its facts.
    std::size_t open_children = 0;

    /// True once the Context object has been destroyed.
    bool ended = false;

    /// The ended Context's journal handles, kept until the teardown actually
    /// happens, so a restart in the meantime still rebuilds this scope — the
    /// scopes opened inside it are subcontexts of it and cannot be rebuilt
    /// without it.
    std::vector<std::uint64_t> replay_handles;
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

/// The kernel a Context was opened on, or KernelError if it no longer exists.
///
/// A Context refers to its Kernel by pointer, and nothing stops the Kernel
/// being destroyed first — a Context with static storage duration outlives
/// shared_kernel() at exit. It used to call into the destroyed object.
Kernel &live_kernel(Kernel *kernel, const std::weak_ptr<const int> &lifetime) {
    if (lifetime.expired()) {
        throw KernelError("the Kernel this Context was opened on no longer exists");
    }
    return *kernel;
}

/// An expression as a request payload: its internal form, sent as structure.
/// The same payload Kernel::eval_tracked and Kernel::remember build from an Expr.
detail::Payload form_of(const Expr &expr) {
    return detail::Payload::form(detail::to_maxima(expr));
}

Expr reply_or_throw(const Reply &reply) {
    auto result = to_expr(reply);
    if (!result) {
        throw MaximaError(result.error().message);
    }
    return std::move(*result);
}

/// Every statement a Context issues changes Maxima's state in a way the
/// kernel's journal accounts for — either recorded here, or undoing something
/// that was. eval_tracked says so, which is what keeps a persistent cache usable
/// for a kernel that uses assumptions.
///
/// Sent as a form, not text: the predicate is the user's, and travelling as
/// structure is what guarantees it cannot be misread on the far side.
Expr evaluate_or_throw(Conversation &conversation, const Expr &form) {
    return reply_or_throw(conversation.eval_tracked(form_of(form)));
}

/// The same, outside a conversation, for questions that change nothing.
Expr evaluate_or_throw(Kernel &kernel, const Expr &form) {
    return reply_or_throw(kernel.eval_tracked(form));
}

Expr call(std::string head, std::vector<Expr> args) {
    return Expr::function(std::move(head), std::move(args));
}

/// True when Maxima's reply contains the symbol `name` — how it reports
/// `redundant` and `inconsistent` from an assume.
bool mentions_symbol(const Expr &expr, std::string_view name) {
    return any_of(expr, [name](const Expr &node) {
        return node.is(Kind::Symbol) && node.name() == name;
    });
}

/// One scope to remove from Maxima, gathered under the registry lock and
/// carried out after it is released, so no lock is held across a round trip.
struct Teardown {
    Kernel *kernel;
    std::weak_ptr<const int> kernel_lifetime;
    std::string name;
    std::string parent;
    std::vector<std::uint64_t> replay_handles;
};

void carry_out(detail::MaximaSession &session, const Teardown &step) {
    // One conversation: the journal forgets the scope and Maxima drops it with
    // no other request in between, so no answer can be computed in one state
    // and filed under the other.
    session.converse_atomically([&step](Conversation &conversation) {
        // Drop the replay entries first: a restart triggered by the teardown
        // itself must not rebuild a scope that is ending.
        for (auto handle = step.replay_handles.rbegin();
             handle != step.replay_handles.rend(); ++handle) {
            conversation.forget(*handle);
        }
        // An assignment has no Expr, so this one line stays as text. Both
        // names are this library's own or Maxima's, never the user's.
        conversation.eval_tracked(detail::Payload::text("context: " + step.parent));
        conversation.eval_tracked(form_of(call("killcontext", {Expr::symbol(step.name)})));
    });
}

} // namespace

std::string_view name_of(Feature feature) {
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
    case Feature::Analytic:
        return "analytic";
    case Feature::Increasing:
        return "increasing";
    case Feature::Decreasing:
        return "decreasing";
    case Feature::OddFun:
        return "oddfun";
    case Feature::EvenFun:
        return "evenfun";
    case Feature::PosFun:
        return "posfun";
    case Feature::IntegerValued:
        return "integervalued";
    case Feature::Commutative:
        return "commutative";
    case Feature::LAssociative:
        return "lassociative";
    case Feature::RAssociative:
        return "rassociative";
    case Feature::Symmetric:
        return "symmetric";
    case Feature::AntiSymmetric:
        return "antisymmetric";
    }
    return "real";
}

// Every change a Context makes to Maxima is one conversation with the change's
// record in the journal. They used to be separate calls — the statement, then
// remember() — and another thread's eval_pure could run in between, compute
// under Maxima's new state and file the answer under the journal's old one,
// in the in-memory cache and in Config::cache_directory alike.

Context::Context(Kernel &kernel)
    : kernel_(&kernel), kernel_lifetime_(kernel.lifetime_),
      name_(next_context_name()) {
    kernel.session().converse_atomically([this](Conversation &conversation) {
        // Whatever is active now becomes this context's parent, which is what
        // makes nesting inherit rather than shadow. Read in the same
        // conversation, so it is still the active context when this one opens.
        const Expr current = evaluate_or_throw(conversation, Expr::symbol("context"));
        parent_ = current.is(Kind::Symbol) ? current.name() : "initial";

        // supcontext rather than newcontext: newcontext would parent the new
        // context on `initial` and so lose the enclosing scope's assumptions.
        const Expr create
            = call("supcontext", {Expr::symbol(name_), Expr::symbol(parent_)});
        evaluate_or_throw(conversation, create);
        replay_handles_.push_back(conversation.remember(form_of(create)));
    });

    const std::lock_guard lock(registry().mutex);
    Scope &scope = registry().scopes[name_];
    scope.parent = parent_;
    scope.kernel = kernel_;
    scope.kernel_lifetime = kernel_lifetime_;
    if (const auto enclosing = registry().scopes.find(parent_);
        enclosing != registry().scopes.end()) {
        ++enclosing->second.open_children;
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
                steps.push_back(
                    {kernel_, kernel_lifetime_, name_, parent_, replay_handles_});
            } else {
                self->second.ended = true;
                self->second.replay_handles = replay_handles_;

                // Innermost first: this scope, then each enclosing scope that
                // had already ended and was only waiting for this one.
                std::string next = name_;
                for (;;) {
                    const auto found = scopes.find(next);
                    if (found == scopes.end() || !found->second.ended
                        || found->second.open_children > 0) {
                        break;
                    }
                    Scope scope = std::move(found->second);
                    scopes.erase(found);
                    steps.push_back({scope.kernel, std::move(scope.kernel_lifetime),
                                     next, scope.parent,
                                     std::move(scope.replay_handles)});

                    const auto enclosing = scopes.find(scope.parent);
                    if (enclosing == scopes.end()) {
                        break;
                    }
                    --enclosing->second.open_children;
                    next = scope.parent;
                }
            }
        }
        for (const Teardown &step : steps) {
            // A kernel that is already gone took its Maxima, and every context
            // in it, with it. There is nothing to tidy, and nothing safe to
            // call.
            if (step.kernel_lifetime.expired()) {
                continue;
            }
            carry_out(step.kernel->session(), step);
        }
    } catch (...) {
    }
}

void Context::assume(const Expr &predicate) {
    Kernel &kernel = live_kernel(kernel_, kernel_lifetime_);
    const Expr statement = call("assume", {predicate});

    bool inconsistent = false;
    kernel.session().converse_atomically([&](Conversation &conversation) {
        const Expr result = evaluate_or_throw(conversation, statement);
        // Maxima answers with a list describing what it did. `inconsistent`
        // means this contradicts something already in force, and Maxima did
        // not add it, so there is nothing to record. `redundant` is harmless:
        // the fact was already implied.
        inconsistent = mentions_symbol(result, "inconsistent");
        if (!inconsistent) {
            replay_handles_.push_back(conversation.remember(form_of(statement)));
        }
    });

    // Carrying on with an inconsistent set of facts would make every later
    // result in this scope meaningless.
    if (inconsistent) {
        throw MaximaError("the assumption " + predicate.str()
                          + " contradicts one already in force");
    }
    assumptions_.push_back(predicate);
}

void Context::declare(const Symbol &symbol, Feature feature) {
    Kernel &kernel = live_kernel(kernel_, kernel_lifetime_);
    const Expr statement
        = call("declare", {symbol, Expr::symbol(std::string(name_of(feature)))});
    kernel.session().converse_atomically([&](Conversation &conversation) {
        evaluate_or_throw(conversation, statement);
        replay_handles_.push_back(conversation.remember(form_of(statement)));
    });
}

std::vector<Expr> Context::facts() const {
    Kernel &kernel = live_kernel(kernel_, kernel_lifetime_);

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
            = evaluate_or_throw(kernel, call("facts", {Expr::symbol(name)}));
        if (result.is(Kind::Function) && result.name() == "list") {
            all.insert(all.end(), result.args().begin(), result.args().end());
        }
    }
    return all;
}

} // namespace proxima
