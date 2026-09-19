# Results and failures

Every operation that can fail for an ordinary reason returns a value saying
so: `proxima::result<T>`, which is `fxt::expected<T, fxt::failure>` from
[FXT](https://github.com/troldal/FXT). `proxima::Failure` *is*
`fxt::failure`, so a Proxima result takes part in any FXT pipeline with
nothing to convert. What Proxima adds is a `proxima::Cause`, carried as the
failure's context, so a program can branch on *why* without reading the
message.

| | |
|---|---|
| `proxima::result<T>`, returned | an ordinary outcome: no closed form (`Cause::NoClosedForm`), nothing solved (`NotSolved`), a fact Maxima needed (`NeedsAssumption`), text that would not parse (`Parse`), an expression with no number in it (`Eval`), Maxima objecting to an argument (`MaximaError`) |
| `proxima::KernelError`, thrown | the conversation broke down: the kernel died, or nothing answered in time |

```cpp
const auto area = proxima::integrate(proxima::exp(proxima::sin(x)), x);
if (!area) {
    std::cerr << area.error().message() << '\n';        // no closed form for ...
    if (proxima::cause_of(area.error()) == proxima::Cause::NeedsAssumption) { ... }
}
```

`*diff(f, x)` reads a result on the spot — `diff` fails only for a malformed
argument — and `proxima::unwrap(r)` throws a failure as the exception its
cause names (`ParseError`, `EvalError`, `OverflowError`, otherwise
`MaximaError`), for code that would rather catch than check. Nothing in the
library throws those of its own accord.

`Expr::parse`, `eval_numeric` and `compile` (the value-returning form of
`Compiled`'s constructor) follow the same rule, with no kernel involved.

## Composing with FXT

Proxima adds no pipe adaptors of its own: its results are FXT's, so FXT's
adaptors apply to them directly, and a chain stops at the first failure.
The operations take their subject first, so a single-argument one lifts as
it is, and one that takes a variable is bound with a lambda or
`std::bind_back`:

```cpp
#include <fxt/monads/AndThen.hpp>
#include <fxt/monads/Transform.hpp>
#include <fxt/monads/ValueOr.hpp>
#include <fxt/utils/Lift.hpp>

const std::string answer
    = proxima::diff(f, x)                                        // result<Expr>
    | fxt::and_then(FXT_LIFT(proxima::factor))                    // one argument: lift it
    | fxt::and_then([&](const proxima::Expr &e) { return proxima::diff(e, x); })
    | fxt::and_then(std::bind_back(FXT_LIFT(proxima::expand), proxima::Env(kernel)))
    | fxt::transform(proxima::to_tex)
    | fxt::value_or(std::string("no answer"));
```

`FXT_LIFT` is variadic, so the defaulted `order` and `kernel` parameters need
no mention; a lambda binds the extra argument anywhere, and `std::bind_back`
binds it last. A chain starts from an operation's result or from
`proxima::result<Expr>{f}`; `fxt::match`, `fxt::with`, `fxt::sequence` and
the rest work on the same values. Include only the headers you use, or
`<fxt.hpp>` for all of them.

[`examples/functional.cpp`](../../../examples/functional.cpp) runs through
the adaptors one at a time: validating with `ensure`, bringing throwing code
in with `attempt`, recovering from a particular `Cause` with `or_else`,
all-or-nothing work with `traverse`, combining independent results with
`with` and `zip`, and consuming one with `match`. [The
how-to](../how-to/handle-failures.md) collects the common patterns.
