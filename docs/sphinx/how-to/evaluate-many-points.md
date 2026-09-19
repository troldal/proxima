# Evaluate an expression at many points

**Task:** tabulate, plot or integrate numerically a formula Maxima produced,
without a round trip per point.

Get the formula once, then compile it:

```cpp
#include <proxima/numeric.hpp>
#include <proxima/ops.hpp>

namespace px = proxima;

const px::Symbol x("x");
const px::Expr F = *px::integrate(pow(x, 2) * px::sin(x), x);

const px::Compiled f(F, x);                 // prepared once
std::vector<double> table;
for (int i = 0; i <= 1000; ++i) {
    table.push_back(f(i * 0.001));          // evaluated a thousand times
}
```

`Compiled` resolves every symbol and function up front, so a call is a pass
over a flat list of instructions — about six times faster than
`eval_numeric` in a loop. It is safe to share between threads.

## Several variables

List the variables once; pass their values in the same order:

```cpp
const px::Symbol y("y");
const std::array variables{x, y};
const px::Compiled g(pow(x, 2) + x * y, variables);

g(std::array{2.0, 3.0});                     // 10
```

## Constants that are not variables

Bind the other symbols when compiling; they are folded in:

```cpp
const px::Symbol k("k");
const px::Compiled h(k * pow(x, 2), x, {{k, 0.5}});   // h(x) = 0.5 x^2

h(4.0);                                                // 8
```

`%pi`, `%e` and the other named constants need no binding.

## Without exceptions

The constructor throws `proxima::EvalError` if the expression cannot be turned
into numbers — an unbound symbol, or a function it does not know.
`proxima::compile` is the same preparation returning a `result<Compiled>`
instead:

```cpp
if (const auto c = px::compile(F, x)) {
    (*c)(1.0);
} else {
    // c.error().message() names the symbol or function
}
```
