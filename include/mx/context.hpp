#pragma once

#include <mx/expr.hpp>
#include <mx/kernel.hpp>
#include <mx/ops.hpp>
#include <mx/symbol.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace mx {

/// A property a symbol can be declared to have.
///
/// Maxima's `declare`. Distinct from an assumption: an assumption is a relation
/// that happens to hold (`x > 0`), a declaration is a standing property of the
/// symbol itself (`n` is an integer).
enum class Feature {
    Integer,
    NonInteger,
    Even,
    Odd,
    Rational,
    Irrational,
    Real,
    Imaginary,
    Complex,
    Constant,
    Prime,
    Increasing,
    Decreasing,
};

/// Maxima's spelling of `feature`.
std::string_view nameOf(Feature feature);

/// A scope of assumptions and declarations.
///
/// Constructing one opens a fresh Maxima context; destroying it discards
/// everything assumed or declared inside. Maxima's own contexts do the work, so
/// this really is a scope and not a best-effort undo: `killcontext` removes the
/// facts *and* the declarations, which a manual `forget` of each assumption
/// would not.
///
/// Contexts nest. A context created while another is active inherits its facts,
/// so an inner scope can add to an outer one without repeating it.
///
/// Scopes need not end innermost first. If an outer Context is destroyed while
/// a scope opened inside it is still open, its facts stay in force for that
/// inner scope — which was opened inheriting them — and are discarded when the
/// last such inner scope ends.
///
/// ## Why assumptions matter more than they look
///
/// Without them Maxima asks. `integrate(x^n, x)` cannot proceed without knowing
/// whether `n` is -1, and over a pipe a question is not something that can be
/// answered — the kernel turns it into an error naming the missing fact. So an
/// operation that fails with "needs an assumption" is telling you precisely
/// what to put in a Context.
///
///     mx::Context ctx;
///     ctx.assume(gt(Expr(n), Expr(-1)));
///     const auto result = mx::integrate(pow(Expr(x), Expr(n)), x);
///
/// ## State, and what happens if the kernel restarts
///
/// Everything done here is registered with the Kernel's replay journal, so a
/// kernel that dies or hangs mid-session comes back with these assumptions
/// still in force. Without that, a restart would silently drop them and later
/// results would be quietly wrong rather than obviously broken.
///
/// The assumptions are also kept in C++ so that PLAN.md step 14's cache key can
/// include them: a result computed under `x > 0` is not the same result as one
/// computed without it.
class Context {
public:
    /// Opens a new Maxima context, nested inside whichever is currently active.
    explicit Context(Kernel &kernel = sharedKernel());

    /// Discards everything assumed or declared in this scope: at once, or, if
    /// a scope opened inside this one is still open, when the last of those
    /// ends.
    ~Context();

    Context(const Context &) = delete;
    Context &operator=(const Context &) = delete;
    Context(Context &&) = delete;
    Context &operator=(Context &&) = delete;

    /// Assumes a relation holds, e.g. `gt(Expr(x), Expr(0))`.
    ///
    /// Throws mx::MaximaError if the assumption contradicts one already in
    /// force — Maxima detects that, and silently carrying on with an
    /// inconsistent set of facts would make every later result meaningless.
    /// A redundant assumption is accepted quietly.
    void assume(const Expr &predicate);

    /// Declares a standing property of a symbol.
    void declare(const Symbol &symbol, Feature feature);

    /// Every fact in force in this scope, innermost first: this context's own
    /// assumptions and declarations, then each enclosing scope's, then those
    /// made outside any Context (Maxima's `initial` context). Maxima's built-in
    /// facts about its own type system, such as `kind(%e, irrational)`, are
    /// left out.
    ///
    /// Describes *this* context whichever is current, so calling it on an
    /// outer scope while an inner one is open lists the outer scope's facts.
    /// The walk outward stops at the first context this library did not open
    /// — one created by hand through Kernel::eval, say — after listing that
    /// context's own facts.
    std::vector<Expr> facts() const;

    /// What was assumed in *this* scope, in the order it was assumed.
    const std::vector<Expr> &assumptions() const { return assumptions_; }

    /// The Maxima context name, for diagnostics.
    const std::string &name() const { return name_; }

private:
    Kernel *kernel_;
    std::string name_;
    std::string parent_;
    std::vector<Expr> assumptions_;

    /// Journal handles for this scope's statements, removed when the scope is
    /// torn down so that a later restart does not resurrect it. For a scope
    /// that ends while an inner one is open, that is later than destruction:
    /// the inner scope cannot be rebuilt without it.
    std::vector<std::uint64_t> replayHandles_;
};

} // namespace mx
