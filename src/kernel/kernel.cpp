#include <proxima/kernel.hpp>

#include "kernel/session.hpp"
#include "wire/from_maxima.hpp"
#include "wire/sexpr.hpp"
#include "wire/to_maxima.hpp"

#include <proxima/errors.hpp>
#include <proxima/expr.hpp>

namespace proxima {

// The special members are defined here rather than in the header because
// detail::MaximaSession is incomplete at the point of declaration.
Kernel::Kernel(Config config)
    : session_(std::make_unique<detail::MaximaSession>(std::move(config))) {}

Kernel::~Kernel() = default;

Kernel::Kernel(Kernel &&) noexcept = default;

Kernel &Kernel::operator=(Kernel &&) noexcept = default;

detail::MaximaSession &Kernel::session() const {
    // Moving a Kernel moves its session, and every method used to dereference
    // the empty pointer left behind. A Context holding a pointer to a Kernel
    // that has since been moved from reaches here too.
    if (!session_) {
        throw KernelError("this Kernel has been moved from and has no Maxima "
                          "session; use the Kernel it was moved into");
    }
    return *session_;
}

// Text becomes an eval_string payload, an Expr a cppread one. Either way the
// session only ever sees a call on a string literal it escaped itself.

Reply Kernel::eval(std::string_view expression) {
    return session().eval(detail::Payload::text(expression));
}

Reply Kernel::eval(const Expr &form) {
    return session().eval(detail::Payload::form(detail::to_maxima(form)));
}

Reply Kernel::eval_pure(std::string_view expression) {
    return session().eval_pure(detail::Payload::text(expression));
}

Reply Kernel::eval_pure(const Expr &form) {
    return session().eval_pure(detail::Payload::form(detail::to_maxima(form)));
}

Reply Kernel::eval_tracked(std::string_view statement) {
    return session().eval_tracked(detail::Payload::text(statement));
}

Reply Kernel::eval_tracked(const Expr &form) {
    return session().eval_tracked(detail::Payload::form(detail::to_maxima(form)));
}

std::expected<Expr, Failure> Kernel::eval_expr(std::string_view expression) {
    return to_expr(eval(expression));
}

std::expected<Expr, Failure> Kernel::eval_expr(const Expr &form) {
    return to_expr(eval(form));
}

std::expected<Expr, Failure> to_expr(const Reply &reply) {
    if (!reply.ok) {
        return std::unexpected(Failure{reply.reason});
    }
    return detail::from_maxima(detail::parse_sexpr(reply.value));
}

void Kernel::invalidate_cache() {
    session().invalidate_cache();
}

Kernel::CacheStats Kernel::cache_stats() const {
    const detail::MaximaSession::CacheStats stats = session().cache_stats();
    return {stats.hits, stats.misses, stats.entries, stats.persistent_hits};
}

std::uint64_t Kernel::remember(std::string_view statement) {
    return session().remember(detail::Payload::text(statement));
}

std::uint64_t Kernel::remember(const Expr &form) {
    return session().remember(detail::Payload::form(detail::to_maxima(form)));
}

void Kernel::forget(std::uint64_t handle) {
    session().forget(handle);
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
