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

### Expressions

`mx::Expr` is an immutable value: copying is a pointer copy, the hash is
computed once, and equality is structural. It works in `std::unordered_map`,
`std::find` and anything else expecting a regular type — and because it is a
local value rather than a handle into the Maxima process, it survives a kernel
restart.

```cpp
const mx::Symbol x("x"), y("y");

mx::Expr f = pow(mx::Expr(x), 2) + 3 * x + 2;   // operators
f = mx::Expr::parse("x^2 + 3*x + 2");           // or infix text, no kernel
```

Ten node kinds — `Integer`, `Rational`, `Real`, `Symbol`, `Add`, `Mul`, `Pow`,
`Function`, `Relation`, `Opaque` — stay that few because two of them absorb
everything else. `Function(head, args)` is an *uninterpreted* application, so
`bessel_j`, a matrix, a derivative or your own `f` need no new type; `Opaque`
holds Maxima source for whatever is not an application at all. A result from
Maxima is never unrepresentable.

**Arithmetic is exact, and unbounded.** `1/3 + 2/5` is `11/15`, not `0.7333…`,
and `2` is not `2.0`. `mx::Integer` has no fixed width, so `30!` is a number you
can compute with rather than a value that wraps or has to be handed back to
Maxima. Values that fit in 64 bits never allocate.

Expressions are **normalised at construction**: nested sums flattened, numeric
terms folded, identities dropped, operands canonically ordered. So `x + 1` and
`1 + x` are equal and hash alike. It is normalisation, not algebra: `x - x`
stays `x - x`, and nothing is expanded or factored, because that is Maxima's
job and having two things simplify would make results depend on which path
produced them.

`str()` renders Maxima-compatible infix, parenthesised by precedence.

### Operations

| | |
|---|---|
| `diff(f, x, n)` | derivative, to any order |
| `integrate(f, x)` / `integrate(f, x, a, b)` | indefinite and definite |
| `limit(f, x, a, side)` | two-sided, or from above or below |
| `solve(equation, x)` | values for one unknown |
| `solve(equations, unknowns)` | a system; one value per unknown, in the order asked for |
| `expand` `factor` `simplify` `subst` | algebraic rearrangement |
| `parse(text)` | Maxima's own parser, for anything the offline one will not take |
| `contains(e, x)` | local; no kernel |

Builders for `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `sinh`, `cosh`, `log`,
`abs`, `exp`, `sqrt`, and the constants `%pi`, `%e`, `%i`, `inf`, `minf`.
Relations are built by name — `eq`, `ne`, `lt`, `le`, `gt`, `ge` — so that `==`
can keep its ordinary meaning.

Results chain, because what comes back is an expression rather than text:

```cpp
mx::expand(mx::factor(mx::diff(f, x)));
```

### Two parsers

`Expr::parse` is a Pratt parser over a subset of Maxima's grammar — arithmetic,
comparisons, function application, lists, strings — and needs **no kernel**.
Statements are refused; it parses expressions, not programs. `mx::parse` hands
the text to Maxima and so accepts everything, at the cost of a round trip.

They differ in more than grammar: **`Expr::parse` parses, `mx::parse` also
evaluates.** `5!` is `factorial(5)` to the first and `120` to the second.

Precedences are Maxima's, including the two that catch people out: `^` is
right-associative (`x^2^3` is `x^(2^3)`) and unary minus binds looser than it
(`-x^2` is `-(x^2)`).

### Numeric evaluation

Once a closed form exists, turning it into numbers is ordinary arithmetic — no
round trip per point.

```cpp
mx::evalNumeric(*integral, {{"x", 1.0}});        // 2.22324, one shot
mx::isEvaluable(e, bindings);                    // ask without catching
```

For repeated evaluation — plotting, root-finding, quadrature — compile once:

```cpp
const mx::Compiled f(*integral, x);
for (int i = 0; i < points; ++i) { plot(f(i * step)); }

const auto g = mx::asFunction(*integral, x);     // same thing, as a std::function
```

`Compiled` resolves every symbol to an argument slot and every function to a
table index up front, and evaluates integer powers by squaring rather than
`std::pow`. About **6× faster** than `evalNumeric` in a loop, and errors surface
when you compile rather than at every point. Thread-safe to share.

`%pi` and friends are recognised; an explicit binding overrides them. An unknown
function is an error rather than a guess.

### Assumptions

`mx::Context` opens a Maxima context and discards it on destruction —
assumptions *and* declarations, which forgetting each assumption individually
would not achieve. Contexts nest and inherit. A contradictory assumption is
refused. `declare` covers `Integer`, `Even`, `Odd`, `Rational`, `Real`,
`Complex`, `Prime`, `Constant` and the rest of Maxima's features.

### The kernel

- **Started on first use**, or constructed explicitly. `sharedKernel()` is the
  process-wide one; every operation takes a `Kernel` defaulting to it.
- **Serialised**, so a `Kernel` is safe to share between threads. For real
  parallelism, give each thread its own — Maxima is one process doing one thing.
- **Survives its own death.** If Maxima hangs or exits, the failing call reports
  it and the kernel is restarted with its assumptions replayed, so the next call
  starts from a working session rather than a wrong one.
- **Remembers answers.** An LRU keyed on the Maxima source, discarded whenever
  anything might have changed it — any raw `eval`, any assumption added or
  dropped. Sized by `Config::cacheEntries`; zero disables it.
- **Optionally between runs.** Set `Config::cacheDirectory` and answers survive
  process exit and are shared with other processes using the same directory.
  Every key carries the Maxima version, this library's version *and* the
  assumption state, so an entry can only be read back under the conditions that
  produced it — `sqrt(x^2)` cached under `assume(x > 0)` is not visible to a
  process that never made the assumption.
- **Cannot be deadlocked by a prompt.** Maxima asks the user for facts it lacks,
  and reads the answer from standard input; over a pipe that would block and
  then swallow the next request. Questions become errors instead — see *When
  Maxima needs a fact it has not been told*.

`Config` covers `maximaRoot`, `timeout`, `startupTimeout`, `cacheEntries`,
`cacheDirectory`, `loadUserInit` and `userDir`. The user's own `maxima-init.mac` is **not** loaded
by default: a library should compute the same answer on every machine.

## Requirements

- A C++23 compiler.
- Maxima built on SBCL, found at runtime.
  - Windows: the official installer. `C:\maxima-5.50.0` or wherever you put it.
  - openSUSE: `zypper install maxima maxima-exec-sbcl`
  - Debian/Ubuntu: `apt install maxima maxima-sbcl`

Nothing else needs installing. The one library dependency —
Boost.Multiprecision, which backs `mx::Integer` — is fetched by CPM at configure
time and built as part of the project, so there is no system package to add and
no version to match. Maxima is not needed to build at all; it is located when a
kernel first starts.

The first configure downloads Boost and takes a minute or so. After that the
sources live in a **shared CPM cache** rather than in each build tree, so the
other presets configure in seconds. The cache defaults to `~/.cache/CPM`; set
`CPM_SOURCE_CACHE`, in the environment or on the command line, to put it
elsewhere.

`cmake --install` installs Boost's headers into the same prefix as this library.
That is deliberate rather than untidy: `mx::Integer` holds a `cpp_int` by value,
so `<mx/integer.hpp>` needs them, and an installed library whose public header
does not compile would be no use. A consumer's `find_package(maxima_cpp)` then
resolves Boost from that prefix — the same Boost this library was compiled
against. Consumers who carry their own Boost should expect it to be found first
only if their `CMAKE_PREFIX_PATH` says so.

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

## When Maxima needs a fact it has not been told

Maxima sometimes needs a fact it has not been told. Asked to integrate `x^n` it
would normally *interrogate the user* — "Is n equal to -1?" — which over a pipe
cannot be answered. The kernel turns the question into an error naming the
missing fact, so it tells you exactly what to supply:

```cpp
const auto stuck = mx::integrate(pow(mx::Expr(x), mx::Expr(n)), x);
stuck.error().message;
// "this computation needs an assumption that was not supplied.
//  Maxima asked: Is n equal to -1?"

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
