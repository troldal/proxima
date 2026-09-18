#pragma once

// The representation behind proxima::Integer, for the library's own sources.
//
// <proxima/integer.hpp> names detail::BigInt without defining it, so Boost
// stays out of every consumer's translation units. This header defines it, and
// is the one way in to an Integer's representation: integer.cpp builds on it,
// and the normaliser uses it to hand exact rational arithmetic to cpp_rational.

#include <proxima/integer.hpp>

#include <boost/multiprecision/cpp_int.hpp>

#include <cstdint>
#include <utility>

namespace proxima::detail {

using Wide = boost::multiprecision::cpp_int;
using WideRational = boost::multiprecision::cpp_rational;

/// A value that does not fit in 64 bits. Never holds one that does: see
/// Integer's layout note.
struct BigInt {
    Wide value;
};

struct IntegerAccess {
    /// The Integer holding `value`, inline if it fits in 64 bits. Every result
    /// of wide arithmetic comes back through here, which is what keeps the
    /// representation canonical.
    static Integer from_wide(Wide value);

    /// A copy of the value as a cpp_int.
    static Wide wide(const Integer &value) {
        return value.big_ ? value.big_->value : Wide(value.small_);
    }

    /// Calls `f` with the value as a `const cpp_int &`, without copying a
    /// large one.
    template <typename F>
    static decltype(auto) with_wide(const Integer &value, F &&f) {
        if (value.big_) {
            return std::forward<F>(f)(value.big_->value);
        }
        const Wide small(value.small_);
        return std::forward<F>(f)(small);
    }

    /// `f(a, b)` with both values as `const cpp_int &`.
    template <typename F>
    static decltype(auto) with_wide(const Integer &a, const Integer &b, F &&f) {
        return with_wide(a, [&](const Wide &wa) -> decltype(auto) {
            return with_wide(b, [&](const Wide &wb) -> decltype(auto) {
                return std::forward<F>(f)(wa, wb);
            });
        });
    }

    static bool is_small(const Integer &value) { return !value.big_; }
    /// The inline value; only meaningful when is_small.
    static std::int64_t small(const Integer &value) { return value.small_; }
};

} // namespace proxima::detail
