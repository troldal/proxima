#include <proxima/traverse.hpp>

#include <proxima/errors.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace proxima {
namespace {

bool is_identifier_part(char c) {
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
bool opaque_mentions(std::string_view text, std::string_view name) {
    if (name.empty()) {
        return false;
    }
    if (!std::all_of(name.begin(), name.end(), is_identifier_part)) {
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
        if (!is_identifier_part(text[at])) {
            ++at;
            continue;
        }
        const std::size_t start = at;
        while (at < text.size() && is_identifier_part(text[at])) {
            ++at;
        }
        if (text.substr(start, at - start) == name) {
            return true;
        }
    }
    return false;
}

} // namespace

Expr detail::with_operands(const Expr &expr, std::vector<Expr> operands) {
    switch (expr.kind()) {
    case Kind::Add:
        return Expr::add(std::move(operands));
    case Kind::Mul:
        return Expr::mul(std::move(operands));
    case Kind::Pow:
        return Expr::pow(std::move(operands.at(0)), std::move(operands.at(1)));
    case Kind::Function:
        return Expr::function(expr.name(), std::move(operands));
    case Kind::Relation:
        return Expr::relation(expr.relation_op(), std::move(operands.at(0)),
                              std::move(operands.at(1)));
    default:
        return expr; // A leaf has no operands to replace.
    }
}

bool contains(const Expr &expr, const Symbol &symbol) {
    return any_of(expr, [&symbol](const Expr &node) {
        if (node.is(Kind::Symbol)) {
            return node.name() == symbol.name();
        }
        // Unmodelled text can still name the symbol, and solve's check that a
        // solution no longer mentions its unknown depends on seeing it there.
        return node.is(Kind::Opaque)
               && opaque_mentions(node.opaque_text(), symbol.name());
    });
}

Expr replace(const Expr &expr, const Symbol &symbol, const Expr &value) {
    return transform(expr, [&symbol, &value](const Expr &node) -> Expr {
        if (node.is(Kind::Symbol) && node.name() == symbol.name()) {
            return value;
        }
        if (node.is(Kind::Opaque)
            && opaque_mentions(node.opaque_text(), symbol.name())) {
            throw Error("cannot replace " + symbol.name()
                        + " inside the unmodelled expression " + node.str()
                        + " without parsing it; proxima::subst has Maxima do it");
        }
        return node;
    });
}

} // namespace proxima
