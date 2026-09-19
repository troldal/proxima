# Solve an equation and get numbers out

**Task:** find the solutions of an equation and use them as `double`s.

Solve exactly first; roots come back as expressions, which you can keep exact,
render, or evaluate:

```cpp
#include <proxima/numeric.hpp>
#include <proxima/ops.hpp>

namespace px = proxima;

const px::Symbol x("x");

std::vector<double> numbers;
if (const auto roots = px::solve(eq(pow(x, 2), 2), x)) {
    for (const px::Expr &root : *roots) {                 // -2^(1/2), 2^(1/2)
        numbers.push_back(*px::eval_numeric(root));       // -1.41421..., 1.41421...
    }
}
```

`eval_numeric` needs no Maxima, and the roots have no free symbols left, so it
needs no bindings either.

## A system of equations

Give the equations and the unknowns; each solution holds a value per unknown,
in the order you asked for them:

```cpp
const px::Symbol y("y");
const std::vector<px::Expr> system{eq(x + y, 3), eq(x - y, 1)};
const std::vector<px::Symbol> unknowns{x, y};

if (const auto found = px::solve(system, unknowns)) {
    for (const px::Solution &s : *found) {
        // s[0] is x = 2, s[1] is y = 1
    }
}
```

## When there is no closed form

Maxima cannot solve every equation symbolically. `solve` then fails with
`Cause::NotSolved`, rather than handing back something that is not a
solution. For a real root in a known interval, ask for it numerically:

```cpp
const px::Expr f = pow(x, 5) - x - 1;              // no solution in radicals
const auto root = px::find_root(f, x, 1.0, 2.0);   // result<double>: 1.1673...
```

`find_root` needs the expression to change sign across the interval. For
polynomials, `nroots(p, a, b)` counts the real roots in `(a, b]` exactly, and
`realroots(p)` isolates all of them.
