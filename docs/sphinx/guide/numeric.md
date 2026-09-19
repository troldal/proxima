# Numeric evaluation

Once a closed form exists, turning it into numbers is ordinary arithmetic — no
round trip per point, and no kernel at all.

```cpp
*proxima::eval_numeric(*integral, {{x, 1.0}});        // 2.22324, one shot
proxima::is_evaluable(e, bindings);                    // ask, without evaluating
```

`Bindings` gives symbols their values, by `Symbol` or by name:
`{{x, 2.0}, {"y", 3.0}}`. `base.with("h", 2.0)` is a copy with one more,
leaving `base` as it was.

## Compile once, evaluate many times

For repeated evaluation — plotting, root-finding, quadrature — compile once:

```cpp
const proxima::Compiled f(*integral, x);
for (int i = 0; i < points; ++i) { plot(f(i * step)); }

const auto g = proxima::as_function(*integral, x);     // the same Compiled, bound to x
```

`Compiled` resolves every symbol to an argument slot and every function to a
table index up front, and evaluates integer powers by squaring rather than
`std::pow`. About **6× faster** than `eval_numeric` in a loop, and errors
surface when you compile rather than at every point. Thread-safe to share.

With several variables, list them once and pass their values in the same
order: `Compiled(e, std::array{x, y})` is called as `f(std::array{1.0, 2.0})`.
`proxima::compile` is the same preparation returning a `result<Compiled>`
instead of throwing.

`%pi` and friends are recognised; an explicit binding overrides them. An
unknown function is an error rather than a guess.

## Which functions

The functions Maxima writes into its answers, under Maxima's names and with
Maxima's meanings:

- **Trigonometric and hyperbolic:** `sin` … `tanh`, their reciprocals
  `sec`, `csc`, `cot`, `sech`, `csch`, `coth`, and all the inverses.
  `integrate(tan(x), x)` is `log(sec(x))`, and evaluates.
- **The rest:**
  - `exp`, `log`, `sqrt`;
  - `gamma`, `factorial`, `double_factorial`, `erf`, `erfc`;
  - `abs`, `signum`, `floor`, `ceiling`, `round`, `mod`, `max`, `min`, `atan2`;
  - `realpart`, `imagpart`, `conjugate`, `cabs`, `carg`.

`proxima::numeric_functions()` lists them. Each has a builder in
`<proxima/functions.hpp>` — `proxima::sec(x)`, `proxima::mod(x, 3)` — so
anything built there can be evaluated. Where a `<cmath>` function of the same
name means something else, Maxima's meaning wins:

- `mod(-7, 3)` is 2;
- `round(2.5)` is 2;
- `acot(-1)` is `-%pi/4`.

Each is checked against Maxima itself in the tests.

Outside a function's real domain the answer is NaN or an infinity, as from
`<cmath>`: `log(-1)` is NaN.

## Complex numbers

Maxima's answers are sometimes complex, even when their value is real: the
roots of `x^2 = -1` are `%i` and `-%i`, and the roots of a cubic often carry
an `%i` that cancels. `eval_complex` evaluates over the complex numbers, with
`%i` the imaginary unit:

```cpp
*proxima::eval_complex(roots[0]);                       // (0,-1)
*proxima::eval_complex(proxima::log(proxima::Expr(-1)));  // (0,3.14159)
```

Each function takes its principal value, on the same side of each branch cut
as Maxima, so `asin(2.0)` is `1.5708 - 1.317 %i`, as it is in Maxima.

A real expression gives exactly the number `eval_numeric` gives, with a zero
imaginary part. A function with no complex meaning — `floor`, `mod`, `gamma`
and the like — evaluates only when its arguments are real. `Compiled` has no
complex counterpart.

## When Maxima knows more

For a function evaluation does not know — `bessel_j`, say — or for Maxima
text held in an `Opaque` node, `to_double` asks Maxima:

```cpp
const px::Expr j0 = px::Expr::function("bessel_j", {px::Expr(0), x});
*px::to_double(j0, {{x, 1.0}});                         // 0.765198, from Maxima
*px::to_double(px::sin(x), {{x, 1.0}});                 // 0.841471, no round trip
```

It evaluates locally whenever it can. Otherwise it has Maxima substitute the
bindings and apply `float`, then evaluates Maxima's answer. `to_complex` is
the same over the complex numbers.

It needs a kernel, and is one question per new point. For many points, first
look for a closed form in functions `Compiled` knows.
