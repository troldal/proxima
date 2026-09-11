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

std::string Kernel::evalRaw(std::string_view statement) {
    return session_->evaluate(std::string(statement));
}

} // namespace mx
