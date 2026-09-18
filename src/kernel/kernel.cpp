#include <proxima/kernel.hpp>

#include "kernel/kernel_internal.hpp"
#include "kernel/reply.hpp"
#include "kernel/session.hpp"
#include "wire/from_maxima.hpp"
#include "wire/sexpr.hpp"
#include "wire/to_maxima.hpp"

#include <proxima/errors.hpp>
#include <proxima/expr.hpp>

#include <string>
#include <utility>
#include <variant>

namespace proxima {
namespace {

/// The payload a query or statement travels as: a cppread of its form, or an
/// eval_string of its text. Either way the session only ever sees a call on
/// a string literal it escaped itself.
detail::Payload payload_of(const std::variant<Expr, std::string> &content) {
    if (const auto *form = std::get_if<Expr>(&content)) {
        return detail::Payload::form(detail::to_maxima(*form));
    }
    return detail::Payload::text(std::get<std::string>(content));
}

Expr call(std::string head, std::vector<Expr> args) {
    return Expr::function(std::move(head), std::move(args));
}

} // namespace

namespace detail {

result<Expr> to_expr(std::string_view wire) {
    return from_maxima(parse_sexpr(wire));
}

Environment environment_for(const Assumptions &assumptions) {
    Environment environment;
    if (assumptions.empty()) {
        return environment;
    }
    // Declarations first, so that a fact can rely on one: `n > 0` for an
    // `n` declared an integer. Both travel as forms, the symbol and the facts
    // being the caller's; the key is the same text, which is canonical
    // because the Assumptions are.
    for (const Declaration &declaration : assumptions.declarations()) {
        const Expr statement
            = call("declare", {declaration.symbol,
                               Expr::symbol(std::string(name_of(declaration.feature)))});
        std::string form = to_maxima(statement);
        environment.key += form;
        environment.key += '\n';
        environment.statements.push_back(
            {Payload::form(form), declaration.symbol.name() + " declared "
                                      + std::string(name_of(declaration.feature))});
    }
    for (const Expr &fact : assumptions.facts()) {
        const Expr statement = call("assume", {fact});
        std::string form = to_maxima(statement);
        environment.key += form;
        environment.key += '\n';
        environment.statements.push_back({Payload::form(form), fact.str()});
    }
    return environment;
}

result<std::string> ask_wire(Kernel &kernel, const Query &query,
                             const Assumptions &assumptions) {
    return to_result(kernel.session().eval_pure(payload_of(query.content_),
                                                environment_for(assumptions)));
}

} // namespace detail

// The special members are defined here rather than in the header because
// detail::MaximaSession is incomplete at the point of declaration.
Kernel::Kernel(Config config)
    : session_(std::make_unique<detail::MaximaSession>(std::move(config))) {}

Kernel::~Kernel() = default;

Kernel::Kernel(Kernel &&) noexcept = default;

Kernel &Kernel::operator=(Kernel &&) noexcept = default;

detail::MaximaSession &Kernel::session() const {
    // Moving a Kernel moves its session, and every method used to dereference
    // the empty pointer left behind.
    if (!session_) {
        throw KernelError("this Kernel has been moved from and has no Maxima "
                          "session; use the Kernel it was moved into");
    }
    return *session_;
}

result<Expr> Kernel::ask(const Query &query, const Assumptions &assumptions) {
    return detail::ask_wire(*this, query, assumptions).and_then(detail::to_expr);
}

result<fxt::unit> Kernel::tell(const Statement &statement) {
    return detail::to_result(session().eval(payload_of(statement.content_)))
        .transform([](const std::string &) { return fxt::unit{}; });
}

void Kernel::invalidate_cache() {
    session().invalidate_cache();
}

Kernel::CacheStats Kernel::cache_stats() const {
    const detail::MaximaSession::CacheStats stats = session().cache_stats();
    return {stats.hits, stats.misses, stats.entries, stats.persistent_hits};
}

void Kernel::set_timeout(std::chrono::milliseconds timeout) {
    session().set_timeout(timeout);
}

bool Kernel::persistence_active() const {
    return session().persistence_active();
}

void Kernel::restart() {
    session().restart();
}

} // namespace proxima
