#pragma once

#include <mx/expr.hpp>

#include <string>

namespace mx {

/// A named unknown.
///
/// A distinct type rather than just an Expr of Kind::Symbol, because the
/// operations that differentiate, solve or take limits are *only* meaningful
/// with respect to a symbol. Taking Symbol in those signatures turns
/// `diff(f, x*2)` from a runtime complaint into a compile error.
///
/// Converts implicitly to Expr, so a Symbol can be used wherever an expression
/// is expected: `x*x + 3*x + 2` works.
class Symbol {
public:
    explicit Symbol(std::string name) : expr_(Expr::symbol(std::move(name))) {}

    const std::string &name() const { return expr_.name(); }

    const Expr &expr() const { return expr_; }
    operator const Expr &() const { return expr_; }

    bool operator==(const Symbol &other) const { return expr_ == other.expr_; }

private:
    Expr expr_;
};

inline namespace literals {

/// `"x"_sym` — a symbol without the ceremony, for expression-heavy code.
inline Symbol operator""_sym(const char *name, std::size_t length) {
    return Symbol(std::string(name, length));
}

} // namespace literals

} // namespace mx
