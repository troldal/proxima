# Write your own renderer

**Task:** print expressions in a notation Proxima does not ship — here, the
prefix notation of Lisp, `(+ 1 x)`.

A renderer is a plain struct with one member function per construct. Each
receives its parts already rendered, and returns the rendered whole. The
library walks the expression, decides where brackets are needed, and calls
you:

```cpp
#include <proxima/render.hpp>

#include <span>
#include <string>
#include <string_view>

struct Prefix {
    std::string integer(const proxima::Integer &n) { return n.to_string(); }
    std::string real(double v) { return std::to_string(v); }
    std::string symbol(std::string_view name) { return std::string(name); }
    std::string verbatim(std::string_view source) { return std::string(source); }

    std::string sum(std::span<const proxima::Term<std::string>> terms) {
        std::string out = "(+";
        for (const auto &term : terms) {
            out += term.negated ? " (- " + term.value + ")" : " " + term.value;
        }
        return out + ")";
    }
    std::string product(std::span<const std::string> factors) {
        return "(*" + joined(factors) + ")";
    }
    std::string fraction(const std::string &n, const std::string &d) {
        return "(/ " + n + " " + d + ")";
    }
    std::string power(const std::string &b, const std::string &e) {
        return "(expt " + b + " " + e + ")";
    }
    std::string call(std::string_view head, std::span<const std::string> args) {
        return "(" + std::string(head) + joined(args) + ")";
    }
    std::string relation(proxima::RelOp op, const std::string &l, const std::string &r) {
        return std::string(op == proxima::RelOp::Equal ? "(= " : "(rel ") + l + " " + r + ")";
    }
    // Prefix notation delimits itself: nothing ever needs grouping.
    std::string group(const std::string &inner) { return inner; }

private:
    static std::string joined(std::span<const std::string> parts) {
        std::string out;
        for (const auto &part : parts) {
            out += " " + part;
        }
        return out;
    }
};

proxima::render(x + 1, Prefix{});                  // (+ 1 x)
proxima::render(pow(x, 2) / 3, Prefix{});          // (/ (expt x 2) 3)
```

Those eleven members are required; the concept `proxima::RendererFor` names
the one that is missing if you forget one. What you return is up to you:
`std::string` here, but a two-dimensional renderer can return boxes with a
width, height and baseline, as
[`examples/text2d.hpp`](../../../examples/text2d.hpp) does.

## Optional members

Define these only when your notation has something better than the default:

- `negate(x)` — a lone minus; otherwise a sum of one negated term.
- `root(radicand, index)` — `√`; otherwise `radicand^(1/index)`.
- `list(items)` — otherwise a call to `list`.
- `postfix(operand, op)` — `x!` and `x!!`; otherwise a call to `factorial`.
- `strength_of(construct)` and `context_for(slot)` — declare that a construct
  carries its own delimiters, as `\frac{}{}` does in TeX, so the library
  stops bracketing inside it.

## Keeping state

`render` works on a copy of the renderer. To read state it gathered — the
packages a TeX renderer needed, say — pass your own object by reference:

```cpp
Prefix mine;
proxima::render(e, std::ref(mine));
```

[Rendering](../guide/rendering.md) explains how the walk decides what to
bracket.
