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
