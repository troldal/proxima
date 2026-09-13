#pragma once

// A two-dimensional text renderer, supplied by the *user* of the library.
//
// Nothing here is part of maxima_cpp. It is a plain struct written against
// <mx/render.hpp> — it inherits nothing, overrides nothing, and the library
// has never heard of it — which is the point of including it in the examples:
// this is what writing your own renderer looks like.
//
// Its output type is a Box rather than a string, because laying out a
// fraction or raising an exponent needs to know each part's width, height and
// baseline before placing it. That is why mx::render is generic over what a
// renderer returns.
//
// Plain ASCII, so it prints anywhere.

#include <mx/render.hpp>

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace text2d {

/// A rectangle of characters, with a baseline: the row the expression "sits
/// on", which is what neighbouring terms align to. For a fraction it is the
/// rule, so the `+` in `a + b/c` lines up with the bar rather than the top.
struct Box {
    std::vector<std::string> rows{""};
    std::size_t baseline = 0;

    std::size_t width() const {
        std::size_t widest = 0;
        for (const std::string &row : rows) {
            widest = std::max(widest, row.size());
        }
        return widest;
    }
    std::size_t height() const { return rows.size(); }
};

inline Box text(std::string_view s) {
    return Box{{std::string(s)}, 0};
}

namespace detail {

/// `box` padded to `width`, with `above` rows over its baseline and `height`
/// rows in all.
inline std::vector<std::string> placed(const Box &box, std::size_t above,
                                       std::size_t height) {
    std::vector<std::string> rows(height, std::string(box.width(), ' '));
    const std::size_t top = above - box.baseline;
    for (std::size_t i = 0; i < box.height(); ++i) {
        std::string row = box.rows[i];
        row.resize(box.width(), ' ');
        rows[top + i] = std::move(row);
    }
    return rows;
}

/// Side by side, baselines aligned.
inline Box beside(const Box &left, const Box &right) {
    const std::size_t above = std::max(left.baseline, right.baseline);
    const std::size_t below = std::max(left.height() - left.baseline,
                                       right.height() - right.baseline);
    const auto a = placed(left, above, above + below);
    const auto b = placed(right, above, above + below);
    Box out;
    out.rows.clear();
    for (std::size_t i = 0; i < a.size(); ++i) {
        out.rows.push_back(a[i] + b[i]);
    }
    out.baseline = above;
    return out;
}

inline Box beside(std::initializer_list<Box> boxes) {
    Box out = text("");
    for (const Box &box : boxes) {
        out = beside(out, box);
    }
    return out;
}

inline std::string centred(const std::string &row, std::size_t width) {
    const std::size_t pad = (width - row.size()) / 2;
    std::string out(pad, ' ');
    out += row;
    out.resize(width, ' ');
    return out;
}

} // namespace detail

struct Renderer {
    Box integer(const mx::Integer &value) { return text(value.toString()); }

    Box real(double value) {
        std::string s = std::to_string(value);
        s.erase(s.find_last_not_of('0') + 1);
        if (!s.empty() && s.back() == '.') {
            s += '0';
        }
        return text(s);
    }

    Box symbol(std::string_view name) {
        // Maxima's constants lose their `%` for display.
        if (!name.empty() && name.front() == '%') {
            name.remove_prefix(1);
        }
        return text(name);
    }

    Box verbatim(std::string_view source) { return text(source); }

    Box sum(std::span<const mx::Term<Box>> terms) {
        Box out = text(terms.empty() ? "0" : "");
        for (std::size_t i = 0; i < terms.size(); ++i) {
            if (i == 0) {
                if (terms[i].negated) {
                    // Spaced when the term is tall: a bare minus beside a
                    // fraction lands on its rule and vanishes into it.
                    out = text(terms[i].value.height() > 1 ? "- " : "-");
                }
            } else {
                out = detail::beside(out, text(terms[i].negated ? " - " : " + "));
            }
            out = detail::beside(out, terms[i].value);
        }
        return out;
    }

    Box product(std::span<const Box> factors) {
        Box out = text("");
        for (std::size_t i = 0; i < factors.size(); ++i) {
            if (i != 0) {
                out = detail::beside(out, text(" "));
            }
            out = detail::beside(out, factors[i]);
        }
        return out;
    }

    /// Numerator over a rule over denominator, centred, baseline on the rule.
    Box fraction(const Box &numerator, const Box &denominator) {
        const std::size_t width
            = std::max(numerator.width(), denominator.width()) + 2;
        Box out;
        out.rows.clear();
        for (const std::string &row : numerator.rows) {
            out.rows.push_back(detail::centred(row, width));
        }
        out.baseline = out.rows.size();
        out.rows.push_back(std::string(width, '-'));
        for (const std::string &row : denominator.rows) {
            out.rows.push_back(detail::centred(row, width));
        }
        return out;
    }

    /// The exponent really is raised above the base.
    Box power(const Box &base, const Box &exponent) {
        Box out;
        out.rows.clear();
        for (const std::string &row : exponent.rows) {
            out.rows.push_back(std::string(base.width(), ' ') + row);
        }
        for (const std::string &row : base.rows) {
            std::string padded = row;
            padded.resize(base.width(), ' ');
            out.rows.push_back(padded);
        }
        out.baseline = base.baseline + exponent.height();
        return out;
    }

    /// A radical sign drawn to the height of what it covers:
    ///
    ///       _______
    ///      /   2
    ///    \/  b  - 4 a c
    Box root(const Box &radicand, unsigned index) {
        const std::size_t h = radicand.height();
        const std::size_t w = radicand.width();
        Box out;
        out.rows.clear();
        // The bar, then the sloped side growing leftwards as it descends.
        out.rows.push_back(std::string(h + 1, ' ') + std::string(w + 1, '_'));
        for (std::size_t i = 0; i < h; ++i) {
            std::string row(h + 1, ' ');
            const bool last = i + 1 == h;
            if (last) {
                row[0] = '\\';
            }
            row[h - i] = '/';
            std::string content = radicand.rows[i];
            content.resize(w, ' ');
            out.rows.push_back(row + " " + content);
        }
        out.baseline = radicand.baseline + 1;
        if (index != 2) {
            Box small = text(std::to_string(index));
            return detail::beside(power(text(""), small), out);
        }
        return out;
    }

    Box call(std::string_view head, std::span<const Box> args) {
        Box inside = text("");
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (i != 0) {
                inside = detail::beside(inside, text(", "));
            }
            inside = detail::beside(inside, args[i]);
        }
        return detail::beside(text(head), group(inside));
    }

    Box list(std::span<const Box> items) {
        Box inside = text("");
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (i != 0) {
                inside = detail::beside(inside, text(", "));
            }
            inside = detail::beside(inside, items[i]);
        }
        return detail::beside({text("["), inside, text("]")});
    }

    Box relation(mx::RelOp op, const Box &lhs, const Box &rhs) {
        return detail::beside(
            {lhs, text(" " + std::string(mx::symbolFor(op)) + " "), rhs});
    }

    /// Brackets that grow with their contents.
    Box group(const Box &inner) {
        if (inner.height() == 1) {
            return detail::beside({text("("), inner, text(")")});
        }
        Box left;
        Box right;
        left.rows.clear();
        right.rows.clear();
        for (std::size_t i = 0; i < inner.height(); ++i) {
            const bool top = i == 0;
            const bool bottom = i + 1 == inner.height();
            left.rows.push_back(top ? "/" : bottom ? "\\" : "|");
            right.rows.push_back(top ? "\\" : bottom ? "/" : "|");
        }
        left.baseline = right.baseline = inner.baseline;
        return detail::beside({left, inner, right});
    }

    // --- the notation delimits itself in three places ---------------------

    mx::Strength strengthOf(mx::Construct construct) {
        // A drawn fraction or radical needs no brackets around it.
        if (construct == mx::Construct::Fraction
            || construct == mx::Construct::Root) {
            return mx::Strength::Atom;
        }
        return mx::defaultStrength(construct);
    }

    mx::Strength contextFor(mx::Slot slot) {
        switch (slot) {
        case mx::Slot::Numerator:
        case mx::Slot::Denominator:
        case mx::Slot::Exponent:
        case mx::Slot::Radicand:
            // Above or below a rule, raised, or under a radical: the layout
            // already shows where the subexpression begins and ends.
            return mx::Strength::Loosest;
        default:
            return mx::defaultContext(slot);
        }
    }

    // negate() is not defined; the library builds it from sum().
};

/// Renders `expr` as rows of text joined by newlines, trailing spaces trimmed.
inline std::string draw(const mx::Expr &expr) {
    const Box box = mx::render(expr, Renderer{});
    std::string out;
    for (std::size_t i = 0; i < box.height(); ++i) {
        std::string row = box.rows[i];
        row.erase(row.find_last_not_of(' ') + 1);
        out += row;
        if (i + 1 != box.height()) {
            out += '\n';
        }
    }
    return out;
}

} // namespace text2d
