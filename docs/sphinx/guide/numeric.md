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
