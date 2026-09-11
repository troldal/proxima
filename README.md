# maxima_cpp

Symbolic mathematics in C++, with [Maxima](https://maxima.sourceforge.io) doing
the work in a child process. No Maxima syntax in the interface, no strings
standing in for expressions.

```cpp
#include <mx/functions.hpp>
#include <mx/numeric.hpp>
#include <mx/ops.hpp>

const mx::Symbol x("x");

const mx::Expr f = pow(mx::Expr(x), 2) * mx::sin(x);

if (const auto integral = mx::integrate(f, x)) {
    std::cout << integral->str() << '\n';                       // cos(x)*(2 - x^2) + 2*x*sin(x)
    std::cout << mx::evalNumeric(*integral, {{"x", 1.0}});      // 2.22324
}
```

## What it does

- **Exact arithmetic.** `1/3 + 2/5` is `11/15`, not `0.7333…`.
- **Symbolic calculus**, from Maxima: `diff`, `integrate` (indefinite and
  definite), `limit`, `solve`, `expand`, `factor`, `simplify`, `subst`.
- **Expressions as values.** `mx::Expr` is immutable, hashable, comparable and
  usable in standard containers. Results chain: `expand(factor(diff(e, x)))`.
- **Assumption scopes.** `mx::Context` opens a Maxima context and discards it on
  destruction, assumptions and declarations alike.
- **Numeric evaluation** without a round trip, once a closed form exists.
- **Two parsers.** `Expr::parse("x^2 - 3*x + 2")` needs no kernel and covers
  ordinary infix; `mx::parse` hands the text to Maxima for anything beyond that.
  Note that the first only parses, while the second also evaluates — `5!` is
  `factorial(5)` to one and `120` to the other.
- **A kernel that survives its own death.** If Maxima hangs or exits, the call
  reports it and the kernel restarts with its assumptions replayed.

## Requirements

- A C++23 compiler.
- Maxima built on SBCL, found at runtime.
  - Windows: the official installer. `C:\maxima-5.50.0` or wherever you put it.
  - openSUSE: `zypper install maxima maxima-exec-sbcl`
  - Debian/Ubuntu: `apt install maxima maxima-sbcl`

Nothing is needed at build time — there are no third-party dependencies, and
Maxima is located when a kernel first starts.

Discovery order: `Config::maximaRoot`, then `$MAXIMA_ROOT`, `$MAXIMA_PREFIX`,
the parent of any `$PATH` entry named `bin`, then the conventional install
locations. A root given explicitly is authoritative: if it is wrong, that is an
error rather than a reason to run some other installation.

## Building

```sh
cmake --preset linux     # or windows, or wsl
cmake --build --preset linux
ctest --preset linux
```

`ctest -LE maxima` runs the two thirds of the suite that need no Maxima at all.

## Installing and consuming

```sh
cmake --install build/linux --prefix /usr/local
```

```cmake
find_package(maxima_cpp 0.1 REQUIRED)
target_link_libraries(my_app PRIVATE mx::maxima_cpp)
```

## Failure is an outcome, not an exception

Three kinds of thing can go wrong, and they are kept apart deliberately.

| | |
|---|---|
| `mx::Failure`, returned in `std::expected` | an ordinary mathematical outcome: no closed form, no solution, unparseable source |
| `mx::MaximaError`, thrown | Maxima objected to an operation with no ordinary way to fail — `diff`, `expand`, `subst`. Means a caller mistake |
| `mx::KernelError`, thrown | the conversation broke down: the kernel died, or nothing answered in time |

```cpp
if (const auto result = mx::integrate(mx::exp(mx::sin(x)), x)) {
    use(*result);
} else {
    std::cerr << result.error().message << '\n';   // no closed form for ...
}
```

## Assumptions

Maxima sometimes needs a fact it has not been told. Asked to integrate `x^n` it
would normally *interrogate the user* — "Is n equal to -1?" — which over a pipe
cannot be answered. The kernel turns the question into an error naming the
missing fact, so it tells you exactly what to supply:

```cpp
mx::integrate(pow(mx::Expr(x), mx::Expr(n)), x);
// no result: this computation needs an assumption that was not supplied.
//            Maxima asked: Is n equal to -1?

mx::Context ctx;
ctx.assume(gt(mx::Expr(n), mx::Expr(0)));
mx::integrate(pow(mx::Expr(x), mx::Expr(n)), x);   // x^(1 + n)*(1 + n)^(-1)
```

The scope ends when `ctx` does, taking the assumption with it — and invalidating
any cached answer that depended on it.

## Notes on the design

`PLAN.md` records the architecture and the reasoning, including the decisions
that shaped it most: that the library owns a C++ expression tree with Maxima as
an oracle rather than holding remote handles; that the wire format is Maxima's
internal s-expressions rather than its display output, because `(%oN)` text is
ambiguous and loses exact rationals; that there is exactly one canonicaliser,
which is why no second symbolic engine is linked in; and that Maxima runs as a
separate process, which is a licensing requirement and not merely a convenience.

## Licence

Maxima is GPL. It is run as a **separate process** communicating over pipes,
which keeps the licences separate — this library is not a derivative work of it.
Embedding Maxima in-process, which is possible via ECL, would pull GPL into the
address space of every consumer. Do not "optimise" the process boundary away.
