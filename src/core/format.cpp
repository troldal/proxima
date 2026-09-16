#include <proxima/expr.hpp>
#include <proxima/integer.hpp>
#include <proxima/mathml.hpp>
#include <proxima/tex.hpp>

#include <ostream>

// Printing to streams and formatting. Kept out of the headers' inline code so
// that including proxima/expr.hpp does not also pull in <ostream> or the renderers.

namespace proxima {

std::ostream &operator<<(std::ostream &out, const Expr &expr) {
    return out << expr.str();
}

std::ostream &operator<<(std::ostream &out, const Integer &value) {
    return out << value.to_string();
}

std::ostream &operator<<(std::ostream &out, Kind kind) {
    return out << kind_name(kind);
}

namespace detail {

std::string notate(const Expr &expr, Notation notation) {
    switch (notation) {
    case Notation::TeX:
        return to_tex(expr);
    case Notation::MathML:
        return to_mathml(expr);
    case Notation::Infix:
        break;
    }
    return expr.str();
}

} // namespace detail

} // namespace proxima
