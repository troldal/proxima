// Every public header, and nothing else, so the checks below see only what a
// consumer's translation unit would.

#include <proxima/config.hpp>
#include <proxima/assumptions.hpp>
#include <proxima/errors.hpp>
#include <proxima/expr.hpp>
#include <proxima/functions.hpp>
#include <proxima/integer.hpp>
#include <proxima/kernel.hpp>
#include <proxima/mathml.hpp>
#include <proxima/numeric.hpp>
#include <proxima/ops.hpp>
#include <proxima/result.hpp>
#include <proxima/render.hpp>
#include <proxima/symbol.hpp>
#include <proxima/tex.hpp>
#include <proxima/traverse.hpp>
#include <proxima/version.hpp>

// No public header may include Boost. proxima::Integer used to hold a cpp_int
// by value, which put Boost.Multiprecision into every consumer's translation
// units — 118,000 preprocessed lines and most of a second of compile time
// each — and made Boost's headers part of the installed package. Every Boost
// header defines BOOST_VERSION, through <boost/config.hpp>.
#ifdef BOOST_VERSION
#error "a public Proxima header includes Boost"
#endif

#include <doctest/doctest.h>

TEST_CASE("the public headers are self-contained and free of Boost") {
    // The real checks are the #error above and this file compiling at all.
    CHECK(sizeof(proxima::Integer) <= 3 * sizeof(void *));
}
