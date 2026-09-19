# Introduction

Proxima is symbolic mathematics in C++23, with
[Maxima](https://maxima.sourceforge.io) doing the work in a child process.
There is no Maxima syntax in the interface and no strings standing in for
expressions: you build expressions with operators, ask for derivatives,
integrals, limits and solutions, and get expressions back — values you can
compare, hash, store, render and evaluate numerically without asking Maxima
again.

## What it is for

Maxima is a mature computer algebra system, and a CAS is exactly what a C++
program cannot easily contain: decades of integration heuristics,
simplification rules and special functions. Proxima lets a C++ program use
that work as if it were a library — for engineering and scientific code that
needs a derivative or a closed form at run time, for tools that generate or
check formulas, and for anything that wants to typeset mathematics it
computed. What stays in C++ is everything that should be fast and local:
building expressions, reading them, rewriting them, rendering them, and
turning them into numbers.

## Design principles

- **Expressions are values, owned in C++.** An `Expr` is an immutable local
  value, not a handle into the Maxima process. Maxima is an oracle: it is
  asked questions and its answers come back as values, so they outlive a
  kernel restart and can be cached, shared between threads and put in
  containers.
- **Structure on the wire, never text.** Expressions travel to Maxima as its
  own internal s-expressions and come back the same way. Maxima's display
  output is ambiguous and loses exact rationals, and anything it has to
  *parse* can fail outside its error trap; neither is on the path.
- **Failure is an outcome.** Every operation returns a `proxima::result<T>`,
  the answer or a failure whose `Cause` says why there is none. Exceptions are
  kept for the conversation with Maxima breaking down.
- **Assumptions travel with the question.** Facts such as `n > 0` are a value
  passed to the operation, not state set up in the kernel beforehand.
  Everything but the kernel is a pure function of its arguments.
- **One canonicaliser.** Expressions are normalised as they are built — sums
  flattened, numbers folded, operands ordered — but algebra is Maxima's job
  alone, so an answer never depends on which of two simplifiers produced it.
- **Extend with plain types.** A renderer of your own is a plain struct
  checked by a concept: no base class, nothing to override.
- **A separate process.** Maxima runs as a child process over pipes. That
  keeps a crash or a hang in Maxima from taking your program with it, and it
  keeps Maxima's GPL licence separate from your code.
- **C++23, and nothing older.** No fallbacks for compilers that lack parts of
  the standard.

[Design](../guide/design.md) and [Architecture](../guide/architecture.md) show
how these shaped the code.

## What is included

| Header | What it gives you |
|---|---|
| `<proxima/expr.hpp>`, `symbol.hpp`, `integer.hpp` | expressions, symbols and exact unbounded integers |
| `<proxima/functions.hpp>` | `sin`, `exp`, `sqrt` and the rest, and constants like `pi()` |
| `<proxima/ops.hpp>` | `diff`, `integrate`, `limit`, `solve`, `ode2`, `sum`, `taylor`, `factor`, `expand`, `is` and more — answered by Maxima |
| `<proxima/assumptions.hpp>` | facts and declarations, as values |
| `<proxima/kernel.hpp>`, `config.hpp` | the Maxima session: caching, recovery, configuration |
| `<proxima/numeric.hpp>` | numbers from expressions, with no round trip: `eval_numeric`, `Compiled` |
| `<proxima/render.hpp>`, `tex.hpp`, `mathml.hpp` | infix text, LaTeX, MathML, and your own renderers |
| `<proxima/traverse.hpp>` | walking and rewriting trees: `nodes`, `fold`, `rewrite`, `replace` |
| `<proxima/result.hpp>`, `errors.hpp` | `result<T>`, `Cause`, and the exceptions |

Results are [FXT](https://github.com/troldal/FXT)'s `expected`, so FXT's
pipeline adaptors apply to them directly.

## Building and installing

### Requirements

- A C++23 compiler, and CMake 3.22 or later. The compiler can be GCC 14 or
  later, Clang 20 or later, MSVC, or clang-cl. Clang 19 cannot compile code
  that uses FXT's adaptors against GCC's standard library.
- Maxima built on SBCL, found when a kernel first starts. Building needs no
  Maxima at all.
  - Windows: the official installer, which includes SBCL.
  - openSUSE: `zypper install maxima maxima-exec-sbcl`.
  - Debian and Ubuntu package Maxima built on GCL, which Proxima cannot use;
    Ubuntu 24.04 has no SBCL build. Use the upstream sources built with SBCL.

  `maxima --list-avail` shows which Lisps an installation was built with.

Everything else — FXT, Boost.Multiprecision and Boost.Process — is fetched by
CPM when you configure, into a shared cache (`~/.cache/CPM`, or
`CPM_SOURCE_CACHE`). The first configure downloads Boost and takes a minute or
so.

### Building

```sh
cmake --preset linux     # or windows, or wsl
cmake --build --preset linux
ctest --preset linux
```

`ctest -LE maxima` runs the two thirds of the suite that need no Maxima.

The example programs in `examples/` and the tests are built only when Proxima
is the top-level project. `-DPROXIMA_BUILD_EXAMPLES=OFF` and
`-DPROXIMA_BUILD_TESTS=OFF` leave them out there too, and `=ON` builds them
when Proxima is a subproject.

### Installing, and using it from your project

```sh
cmake --install build/linux --prefix /usr/local
```

```cmake
find_package(proxima 0.1 REQUIRED)
target_link_libraries(my_app PRIVATE proxima::proxima)
```

Or build it from source as part of your project, with FetchContent (or CPM,
which works the same way):

```cmake
include(FetchContent)
FetchContent_Declare(proxima
        GIT_REPOSITORY https://github.com/troldal/proxima.git
        GIT_TAG master)
FetchContent_MakeAvailable(proxima)
target_link_libraries(my_app PRIVATE proxima::proxima)
```

As a subproject it builds the library alone, without its tests and examples.

The installed tree carries FXT's headers and the Boost.Process library
Proxima was built with, so a consumer needs nothing else. [Getting
started](getting-started.md) walks through a first project.

### Documentation

This site is built from `docs/sphinx`: install Doxygen and
`pip install -r docs/sphinx/requirements.txt`, then
`cmake --build <build> --target docs`.

## A first look

```cpp
#include <proxima/functions.hpp>
#include <proxima/numeric.hpp>
#include <proxima/ops.hpp>

#include <print>

namespace px = proxima;

int main() {
    const px::Symbol x("x");
    const px::Expr f = pow(x, 2) * px::sin(x);

    if (const auto integral = px::integrate(f, x)) {
        std::println("{}", *integral);                               // cos(x)*(2 - x^2) + 2*x*sin(x)
        std::println("{}", *px::eval_numeric(*integral, {{x, 1.0}}));   // 2.2232442754839328
        std::println("{:tex}", *integral);                           // \cos\left(x\right) \left(2 - x^{2}\right) + ...
    }
}
```

`integrate` asks Maxima and returns a `result<Expr>`: the `if` tests whether
there is an answer and `*` reads it. `eval_numeric` needs no Maxima, and
neither does rendering.
