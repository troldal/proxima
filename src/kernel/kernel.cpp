#include <mx/kernel.hpp>

#include "kernel/session.hpp"

namespace mx {

// The special members are defined here rather than in the header because
// detail::MaximaSession is incomplete at the point of declaration.
Kernel::Kernel(Config config)
    : session_(std::make_unique<detail::MaximaSession>(std::move(config))) {}

Kernel::~Kernel() = default;

Kernel::Kernel(Kernel &&) noexcept = default;

Kernel &Kernel::operator=(Kernel &&) noexcept = default;

Reply Kernel::eval(std::string_view expression) {
    return session_->eval(expression);
}

Reply Kernel::evalPure(std::string_view expression) {
    return session_->evalPure(expression);
}

void Kernel::invalidateCache() {
    session_->invalidateCache();
}

Kernel::CacheStats Kernel::cacheStats() const {
    const detail::MaximaSession::CacheStats stats = session_->cacheStats();
    return {stats.hits, stats.misses, stats.entries};
}

std::uint64_t Kernel::remember(std::string statement) {
    return session_->remember(std::move(statement));
}

void Kernel::forget(std::uint64_t handle) {
    session_->forget(handle);
}

void Kernel::setTimeout(std::chrono::milliseconds timeout) {
    session_->setTimeout(timeout);
}

} // namespace mx
