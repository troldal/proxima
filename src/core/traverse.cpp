#include <mx/traverse.hpp>

#include <mx/errors.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mx {
namespace {

bool isIdentifierPart(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
           || c == '_' || c == '%';
}

/// Whether Opaque source text mentions `name`.
///
/// The text is Maxima source this library never parsed, so it is read the way
/// a careful eye would read it rather than parsed. A plain name counts where it
/// stands as a whole identifier — not inside a longer one (`xy`, `x_1`), and
/// not inside a string literal. A name that cannot be written as a plain
/// identifier, such as one with a space, which Maxima source writes with a
/// backslash (`x\ y`), counts wherever its characters appear once the
/// backslashes are dropped.
///
/// Where it errs, it errs towards "yes", which is the direction solve's guard
/// needs: a false yes rejects a solution that was fine, a false no would
/// accept one that is not.
bool opaqueMentions(std::string_view text, std::string_view name) {
    if (name.empty()) {
        return false;
    }
    if (!std::all_of(name.begin(), name.end(), isIdentifierPart)) {
        std::string unescaped;
        unescaped.reserve(text.size());
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\\' && i + 1 < text.size()) {
                ++i;
            }
            unescaped.push_back(text[i]);
        }
        return unescaped.find(name) != std::string::npos;
    }

    std::size_t at = 0;
    while (at < text.size()) {
        if (text[at] == '"') {
            // A string literal, escapes and all, mentions nothing.
            ++at;
            while (at < text.size() && text[at] != '"') {
                at += text[at] == '\\' ? std::size_t{2} : std::size_t{1};
            }
            ++at;
            continue;
        }
        if (!isIdentifierPart(text[at])) {
            ++at;
            continue;
        }
        const std::size_t start = at;
        while (at < text.size() && isIdentifierPart(text[at])) {
            ++at;
        }
        if (text.substr(start, at - start) == name) {
            return true;
        }
    }
    return false;
}

/// `expr` rebuilt with new operands, through the builder for its kind, so it
/// is normalised exactly as a freshly built expression would be.
Expr withOperands(const Expr &expr, std::vector<Expr> operands) {
    switch (expr.kind()) {
    case Kind::Add:
        return Expr::add(std::move(operands));
    case Kind::Mul:
        return Expr::mul(std::move(operands));
    case Kind::Pow:
        return Expr::pow(std::move(operands[0]), std::move(operands[1]));
    case Kind::Function:
        return Expr::function(expr.name(), std::move(operands));
    case Kind::Relation:
        return Expr::relation(expr.relationOp(), std::move(operands[0]),
                              std::move(operands[1]));
    default:
        return expr; // A leaf has no operands to replace.
    }
}

/// replace, reporting whether anything changed, so an untouched subtree is
/// returned as it is without being compared.
Expr replaceIn(const Expr &expr, const Symbol &symbol, const Expr &value,
               bool &changed) {
    switch (expr.kind()) {
    case Kind::Symbol:
        if (expr.name() == symbol.name()) {
            changed = true;
            return value;
        }
        return expr;
    case Kind::Opaque:
        if (opaqueMentions(expr.opaqueText(), symbol.name())) {
            throw Error("cannot replace " + symbol.name()
                        + " inside the unmodelled expression " + expr.str()
                        + " without parsing it; mx::subst has Maxima do it");
        }
        return expr;
    case Kind::Integer:
    case Kind::Rational:
    case Kind::Real:
        return expr;
    default:
        break;
    }

    const std::vector<Expr> &operands = expr.args();
    std::vector<Expr> replaced;
    replaced.reserve(operands.size());
    bool anyChanged = false;
    for (const Expr &operand : operands) {
        replaced.push_back(replaceIn(operand, symbol, value, anyChanged));
    }
    if (!anyChanged) {
        return expr;
    }
    changed = true;
    return withOperands(expr, std::move(replaced));
}

} // namespace

bool contains(const Expr &expr, const Symbol &symbol) {
    if (expr.is(Kind::Symbol)) {
        return expr.name() == symbol.name();
    }
    if (expr.is(Kind::Opaque)) {
        // Unmodelled text can still name the symbol, and solve's check that a
        // solution no longer mentions its unknown depends on seeing it there.
        return opaqueMentions(expr.opaqueText(), symbol.name());
    }
    for (const Expr &operand : expr.args()) {
        if (contains(operand, symbol)) {
            return true;
        }
    }
    return false;
}

Expr replace(const Expr &expr, const Symbol &symbol, const Expr &value) {
    bool changed = false;
    return replaceIn(expr, symbol, value, changed);
}

} // namespace mx
