#include <proxima/expr.hpp>
#include <proxima/integer.hpp>
#include <proxima/mathml.hpp>
#include <proxima/result.hpp>
#include <proxima/tex.hpp>

#include <ostream>

// Printing to streams and formatting. Kept out of the headers' inline code so
// that including proxima/expr.hpp does not also pull in <ostream> or the renderers.

namespace proxima {

std::string_view to_string(Cause cause) {
    switch (cause) {
    case Cause::Unknown:
        return "unknown";
    case Cause::MaximaError:
        return "Maxima error";
    case Cause::NeedsAssumption:
        return "needs an assumption";
    case Cause::Inconsistent:
        return "inconsistent assumptions";
    case Cause::NoClosedForm:
        return "no closed form";
    case Cause::NotSolved:
        return "not solved";
    case Cause::NoLimit:
        return "no limit";
    case Cause::UnexpectedAnswer:
        return "unexpected answer";
    case Cause::Parse:
        return "parse";
    case Cause::Eval:
        return "eval";
    case Cause::Argument:
        return "argument";
    case Cause::Overflow:
        return "overflow";
    }
    return "unknown";
}

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
