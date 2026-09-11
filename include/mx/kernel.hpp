#pragma once

#include <mx/config.hpp>
#include <mx/reply.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
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
/// Thread-safe: calls are serialised, so concurrent callers take turns rather
/// than interleaving requests on one pipe. That makes a Kernel safe to share,
/// not fast to share — Maxima itself is one process doing one thing at a time.
/// For genuine parallelism, give each thread its own Kernel.
///
/// Survives its own death. If Maxima hangs or exits, the failing call reports
/// it and the kernel is restarted behind the scenes with its remembered state
/// replayed, so the next call starts from a working session.
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

    /// Records a statement to replay if the kernel has to be restarted, and
    /// returns a handle for removing it again.
    ///
    /// Maxima is a separate process holding mutable state — assumptions,
    /// declarations, bindings — that a restart would otherwise silently lose,
    /// making later results quietly wrong rather than obviously broken. Scoped
    /// state such as mx::Context registers itself here; there is rarely a
    /// reason to call this directly.
    std::uint64_t remember(std::string statement);

    /// Stops replaying the statement `handle` names.
    void forget(std::uint64_t handle);

    /// Changes the per-call deadline for this kernel. Config::startupTimeout,
    /// which governs launching and restarting, is unaffected.
    void setTimeout(std::chrono::milliseconds timeout);

private:
    std::unique_ptr<detail::MaximaSession> session_;
};

} // namespace mx
