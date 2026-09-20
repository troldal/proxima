# Proxima

*Formerly maxima_cpp.*

Symbolic mathematics in C++, with [Maxima](https://maxima.sourceforge.io) doing
the work in a child process. No Maxima syntax in the interface, no strings
standing in for expressions.

```cpp
#include <proxima/functions.hpp>
#include <proxima/numeric.hpp>
#include <proxima/ops.hpp>

namespace px = proxima;

const px::Symbol x("x");
const px::Expr f = pow(x, 2) * px::sin(x);

if (const auto integral = px::integrate(f, x)) {
    std::cout << *integral << '\n';                          // cos(x)*(2 - x^2) + 2*x*sin(x)
    std::cout << *px::eval_numeric(*integral, {{x, 1.0}});   // 2.22324
}
```

A `Symbol` is an expression wherever one is wanted, so `pow(x, 2)`,
`x * x + 3 * x + 2` and `gt(x, 0)` need no conversion spelled out. The
operations return a `proxima::result` — the answer, or why there is none —
which is what the `if` and the `*` above are reading; see
[Results and failures](docs/sphinx/guide/results.md).

## What it does

- **Expressions are values.** `proxima::Expr` is immutable, hashes once, and
  compares structurally, so it works in any standard container. Arithmetic is
  exact and unbounded: `1/3 + 2/5` is `11/15`, and `30!` is a number. Read a
  tree with `e.match(...)`, which the compiler checks covers every kind.
- **The operations you would expect** — `diff`, `integrate`, `limit`, `solve`,
  `ode2`, `sum`, `taylor`, `factor`, `expand`, `is` and more — each returning
  a `proxima::result<T>`: the answer, or a `Failure` whose `Cause` says why
  there is none.
- **Assumptions are a value**, passed with the question:
  `integrate(pow(x, n), x, assuming(gt(n, 0)))`. Nothing is set up beforehand
  and nothing is left behind.
- **Two parsers**: `Expr::parse`, offline, for a subset of Maxima's syntax;
  `proxima::parse`, Maxima's own, for the rest.
- **Rendering**: infix text, LaTeX and MathML, and a small interface for your
  own renderer. No kernel involved.
- **Numbers without round trips**: `eval_numeric` once, `Compiled` for
  thousands of points.
- **A kernel that keeps going**: thread-safe, restarted if Maxima dies or
  hangs, never deadlocked by a question Maxima wants to ask, and caching
  answers in memory and, if asked, on disk.
- **Composes with [FXT](https://github.com/troldal/FXT)**: its results are
  FXT's, so `f | fxt::and_then(...)` pipelines work directly.

The [user guide](docs/sphinx/guide/expressions.md) covers each of these in
turn.

## Requirements

- A C++23 compiler. GCC 14 or later, or Clang 20 or later; with Clang 19,
  code using FXT's adaptors, as `examples/functional.cpp` does, fails to
  compile against GCC's standard library. MSVC and clang-cl work too.
- Maxima built on SBCL, found at runtime.
  - Windows: the official installer. `C:\maxima-5.50.0` or wherever you put it,
    including a path with non-ASCII characters, on any volume.
  - openSUSE: `zypper install maxima maxima-exec-sbcl`
  - Debian and Ubuntu package Maxima built on GCL, which Proxima cannot use;
    Ubuntu 24.04 has no SBCL build. Use the upstream sources built with SBCL.

Nothing else needs installing. The library depends on
[FXT](https://github.com/troldal/FXT), whose `fxt::failure` and
`fxt::expected` are its result type, and on two parts of Boost —
Boost.Multiprecision, which backs `proxima::Integer`'s large values, and
Boost.Process (v2), which starts the Maxima child and talks to it over
Boost.Asio pipes. All are fetched by CPM at configure time, so there is no
system package to add and no version to match. Maxima is not needed to build
at all; it is located when a kernel first starts.

FXT is in the public headers, being the result type; Boost is not, so code
that includes `<proxima/expr.hpp>` compiles without it.

The first configure downloads Boost and takes a minute or so. After that the
sources live in a **shared CPM cache** rather than in each build tree, so the
other presets configure in seconds. The cache defaults to `~/.cache/CPM`; set
`CPM_SOURCE_CACHE`, in the environment or on the command line, to put it
elsewhere.

`cmake --install` installs FXT's headers and Boost into the same prefix as
this library. That is deliberate rather than untidy: `<proxima/result.hpp>`
includes FXT, so a consumer needs its headers on the include path, and gets
them from the same directory as Proxima's own; and `proxima` is a static
library, so a consumer's executable links the Boost.Process it was built with.
A consumer's `find_package(proxima)` resolves Boost from that prefix — the
same Boost this library was compiled against — and asks for Boost.Process
alone; nothing of Boost reaches the consumer's include path. Consumers who
carry their own FXT or Boost should expect it to be found first only if their
`CMAKE_PREFIX_PATH` says so.

Discovery order: `Config::sbcl_exe` and `Config::maxima_core`, which name the
installation outright and end the search; else `Config::maxima_root`, then
`$MAXIMA_ROOT`, `$MAXIMA_PREFIX`, the parent of any `$PATH` entry named `bin`,
then the conventional install locations. A root given explicitly is
authoritative: if it is wrong, that is an error rather than a reason to run
some other installation. Under a root, where the core lies is asked of the
installation (`maxima -d`) rather than inferred from the layout.

## Building

```sh
cmake --preset linux     # or windows, or wsl
cmake --build --preset linux
ctest --preset linux
```

`ctest -LE maxima` runs the two thirds of the suite that need no Maxima at all.
The `linux-sanitize` preset (`wsl-sanitize` on WSL) builds and tests the same
under AddressSanitizer and UndefinedBehaviorSanitizer, as CI does.

Three example programs are built alongside the library:
- `demo` is a one-screen showcase.
- `tour` is the long version and doubles as a quick-start guide. It walks
  through every public feature in numbered sections, commented throughout, and
  its first part needs no Maxima at all. Read
  [`examples/tour.cpp`](examples/tour.cpp) with its output beside it.
- `functional` composes Proxima's results with FXT's adaptors.

The examples and the tests are built only when Proxima is the top-level
project, so a project that pulls it in with `add_subdirectory`, FetchContent
or CPM builds the library alone. `-DPROXIMA_BUILD_EXAMPLES=ON|OFF` and
`-DPROXIMA_BUILD_TESTS=ON|OFF` override that either way.

## Installing and consuming

```sh
cmake --install build/linux --prefix /usr/local
```

```cmake
find_package(proxima REQUIRED)
target_link_libraries(my_app PRIVATE proxima::proxima)
```

To require a version, write it: `find_package(proxima 0.2 REQUIRED)`. Proxima
follows [semantic versioning](https://semver.org), so before 1.0 each minor
release may change the API, and that accepts any 0.2.x but not 0.3. What
changed between releases is in the [changelog](CHANGELOG.md).

Or build it from source as part of your project:

```cmake
include(FetchContent)
FetchContent_Declare(proxima
        GIT_REPOSITORY https://github.com/troldal/proxima.git
        GIT_TAG master)
FetchContent_MakeAvailable(proxima)
target_link_libraries(my_app PRIVATE proxima::proxima)
```

CI builds [a small consumer project](tests/consumer) both ways, on Linux and
Windows.

## Documentation

- [`docs/sphinx`](docs/sphinx) — the documentation:
  [introduction](docs/sphinx/intro/introduction.md),
  [getting started](docs/sphinx/intro/getting-started.md), a user guide, how-to
  guides, and an API reference generated from the headers by Doxygen and
  Breathe, in the Furo theme. The pages are Markdown, readable here; to build
  the site, install Doxygen and `pip install -r docs/sphinx/requirements.txt`,
  then `cmake --build <build> --target docs`.
- [`ARCH.md`](ARCH.md) — the architecture on one page: the layers, and how a
  question travels from your code to Maxima and back.
- [`DESIGN.md`](DESIGN.md) — the main types and how they relate: who owns,
  shares, produces and calls whom.
- [`examples/tour.cpp`](examples/tour.cpp) — a runnable walk through every
  public feature; read it with its output beside it.
- [`examples/functional.cpp`](examples/functional.cpp) — Proxima in a
  functional style: its results composed with FXT's adaptors, from
  `and_then` to `traverse`, `zip` and `match`.
- [`docs/design.md`](docs/design.md) — how the library came to be as it is,
  and why: the architecture and every decision that shaped it.
- [`ROADMAP.md`](ROADMAP.md) — the plan from 0.1 to 1.0, and what 1.0 will
  promise.
- [`TODO.md`](TODO.md) — the reviews of the code, what they found, and what
  was done about it.

Naming follows the standard library: snake_case for functions, variables and
members, PascalCase for types and enumerators, `kPascalCase` for constants.
`.clang-tidy` enforces it.

## Licence

Proxima is released under the MIT licence; see [LICENSE](LICENSE).

Maxima is GPL. It is run as a **separate process** communicating over pipes,
which keeps the licences separate — this library is not a derivative work of it.
Embedding Maxima in-process, which is possible via ECL, would pull GPL into the
address space of every consumer. Do not "optimise" the process boundary away.
