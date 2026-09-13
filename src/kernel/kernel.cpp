#include <mx/kernel.hpp>

#include "kernel/session.hpp"
#include "wire/to_maxima.hpp"

#include <mx/errors.hpp>
#include <mx/expr.hpp>

namespace mx {

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
    return session().eval(detail::Payload::form(detail::toMaxima(form)));
}

Reply Kernel::evalPure(std::string_view expression) {
    return session().evalPure(detail::Payload::text(expression));
}

Reply Kernel::evalPure(const Expr &form) {
    return session().evalPure(detail::Payload::form(detail::toMaxima(form)));
}

Reply Kernel::evalTracked(std::string_view statement) {
    return session().evalTracked(detail::Payload::text(statement));
}

Reply Kernel::evalTracked(const Expr &form) {
    return session().evalTracked(detail::Payload::form(detail::toMaxima(form)));
}

void Kernel::invalidateCache() {
    session().invalidateCache();
}

Kernel::CacheStats Kernel::cacheStats() const {
    const detail::MaximaSession::CacheStats stats = session().cacheStats();
    return {stats.hits, stats.misses, stats.entries, stats.persistentHits};
}

std::uint64_t Kernel::remember(std::string statement) {
    return session().remember(detail::Payload::text(statement));
}

std::uint64_t Kernel::remember(const Expr &form) {
    return session().remember(detail::Payload::form(detail::toMaxima(form)));
}

void Kernel::forget(std::uint64_t handle) {
    session().forget(handle);
}

void Kernel::setTimeout(std::chrono::milliseconds timeout) {
    session().setTimeout(timeout);
}

bool Kernel::persistenceActive() const {
    return session().persistenceActive();
}

void Kernel::restart() {
    session().restart();
}

} // namespace mx
