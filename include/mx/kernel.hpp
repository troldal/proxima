#pragma once

#include <mx/config.hpp>
#include <mx/reply.hpp>

#include <memory>
#include <string_view>

namespace mx {

namespace detail {
class MaximaSession;
}

/// A persistent Maxima session.
///
/// The session is started on construction and shut down on destruction, so a
/// single Kernel serves many queries without paying process startup each time.
///
/// This is the whole public surface for now. It still speaks Maxima's own
/// syntax and hands back the text of Maxima's internal s-expressions;
/// structured expressions arrive with the term layer (PLAN.md steps 7-9), at
/// which point eval becomes an escape hatch rather than the main entry point.
///
/// Not thread-safe. Serialisation, timeouts and transparent restart after a
/// hang are PLAN.md step 13.
class Kernel {
public:
    explicit Kernel(Config config = {});
    ~Kernel();

    Kernel(Kernel &&) noexcept;
    Kernel &operator=(Kernel &&) noexcept;

    Kernel(const Kernel &) = delete;
    Kernel &operator=(const Kernel &) = delete;

    /// Evaluates one Maxima *expression* — `integrate(x^2, x)`, with no
    /// trailing `;` or `$`, since the expression is substituted into a wrapper
    /// that supplies its own terminator.
    ///
    /// A Maxima error comes back as a Reply with `ok == false` and a reason,
    /// because failing to integrate something is an ordinary outcome. Only
    /// infrastructure failures throw: see mx::KernelError.
    Reply eval(std::string_view expression);

private:
    std::unique_ptr<detail::MaximaSession> session_;
};

} // namespace mx
