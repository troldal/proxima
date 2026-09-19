# Operations

The operations ask Maxima. Each takes its subject first and, last, an optional
[`Env`](assumptions.md): the kernel to ask and the assumptions to ask under.

| | |
|---|---|
| `diff(f, x, n)` | derivative, to any order |
| `integrate(f, x)` / `integrate(f, x, a, b)` | indefinite and definite |
| `limit(f, x, a, side)` | two-sided, or from above or below |
| `solve(equation, x)` | values for one unknown |
| `solve(equations, unknowns)` | a system; one value per unknown, in the order asked for |
| `expand` `factor` `ratsimp` `subst` | algebraic rearrangement (`simplify` is an older name for `ratsimp`) |
| `trigsimp` `trigexpand` `radcan` `partfrac(f, x)` `coeff(f, x, n)` | more rearrangement |
| `taylor(f, x, a, n)` | Taylor expansion, as an ordinary expression |
| `sum(t, k, a, b)` `product(t, k, a, b)` | closed forms; `Cause::NoClosedForm` when there is none |
| `ode2(equation, y, x)` | first- and second-order ODEs, written with `derivative(y, x)` |
| `is(predicate)` | `Truth::True`, `False` or `Unknown` under the assumptions in force |
| `to_float(f)` `nroots(p, a, b)` `realroots(p)` `find_root(f, x, a, b)` | numbers from Maxima: floats, root counts, isolated and numeric roots |
| `parse(text)` | Maxima's own parser, for anything the offline one will not take; see [Parsing](parsing.md) |

A few functions in the same header need no kernel: `lhs(r)` and `rhs(r)`, the
sides of a relation. For walking and rewriting trees locally, see
[Traversal](traversal.md).

Every operation returns a `proxima::result<T>` — `fxt::expected<T,
fxt::failure>` — so `*diff(f, x)` is the derivative and `diff(f, x) |
fxt::and_then(...)` is a pipeline; see [Results and failures](results.md).

## Results chain

Results chain, because what comes back is an expression rather than text —
and a chain of operations stops at the first that fails:

```cpp
proxima::diff(f, x) | fxt::and_then(FXT_LIFT(proxima::factor))
                    | fxt::and_then(FXT_LIFT(proxima::expand));
```

For a Maxima function this library does not wrap, ask the kernel directly:
see [the how-to](../how-to/call-unwrapped-maxima.md).
