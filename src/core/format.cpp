#include <mx/expr.hpp>
#include <mx/integer.hpp>
#include <mx/mathml.hpp>
#include <mx/tex.hpp>

#include <ostream>

// Printing to streams and formatting. Kept out of the headers' inline code so
// that including mx/expr.hpp does not also pull in <ostream> or the renderers.

namespace mx {

std::ostream &operator<<(std::ostream &out, const Expr &expr) {
    return out << expr.str();
}

std::ostream &operator<<(std::ostream &out, const Integer &value) {
    return out << value.toString();
}

std::ostream &operator<<(std::ostream &out, Kind kind) {
    return out << kindName(kind);
}

namespace detail {

std::string notate(const Expr &expr, Notation notation) {
    switch (notation) {
    case Notation::TeX:
        return toTeX(expr);
    case Notation::MathML:
        return toMathML(expr);
    case Notation::Infix:
        break;
    }
    return expr.str();
}

} // namespace detail

} // namespace mx
