#pragma once

#include <mx/config.hpp>

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
/// syntax and returns its result text verbatim; structured expressions arrive
/// with the term layer (PLAN.md steps 7-9), at which point evalRaw becomes an
/// escape hatch rather than the main entry point.
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

    /// Evaluates one Maxima statement, which must end in ';' or '$'.
    ///
    /// Returns the text of the result, or an empty string for a statement that
    /// produces no output (one terminated by '$'). Throws KernelError if the
    /// session could not be reached.
    std::string evalRaw(std::string_view statement);

private:
    std::unique_ptr<detail::MaximaSession> session_;
};

} // namespace mx
