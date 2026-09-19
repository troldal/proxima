# Getting started

This page takes you from nothing to a program that differentiates, integrates,
solves and evaluates, one step at a time. Each step adds a few lines to the
same `main.cpp`.

## 1. Install the prerequisites

You need a C++23 compiler, CMake 3.22 or later, and Maxima built on SBCL (see
[Requirements](introduction.md#requirements)). Check the Maxima:

```sh
maxima --list-avail
```

It should list `sbcl` among the Lisps it was built with.

## 2. Build and install Proxima

```sh
git clone https://github.com/troldal/proxima.git
cd proxima
cmake --preset linux                    # or windows
cmake --build --preset linux
cmake --install build/linux --prefix "$HOME/.local"
```

## 3. Create a project

Two files. `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.22)
project(hello_proxima LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(proxima REQUIRED)

add_executable(hello main.cpp)
target_link_libraries(hello PRIVATE proxima::proxima)
```

and `main.cpp`, which for now only builds an expression and prints it:

```cpp
#include <proxima/expr.hpp>
#include <proxima/functions.hpp>

#include <print>

namespace px = proxima;

int main() {
    const px::Symbol x("x");
    const px::Expr f = pow(x, 3) - 2 * x + 1;

    std::println("f = {}", f);                  // f = 1 + x^3 - 2*x
}
```

Configure it with the prefix you installed to, and run it:

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="$HOME/.local"
cmake --build build
./build/hello
```

Nothing here needed Maxima: expressions are ordinary C++ values. They are
normalised as they are built, which is why the terms come out in a canonical
order.

## 4. Ask Maxima a question

Include `<proxima/ops.hpp>` and ask for a derivative and an integral:

```cpp
#include <proxima/ops.hpp>

// ... in main:
    const auto df = px::diff(f, x);             // result<Expr>
    std::println("f' = {}", *df);               // f' = 3*x^2 - 2

    if (const auto F = px::integrate(f, x)) {
        std::println("F = {}", *F);             // F = x - x^2 + x^4/4
    }
```

The first call starts Maxima in the background; later calls reuse it. Every
operation returns a `proxima::result<Expr>`: the answer, or a failure saying
why there is none. `*` reads the answer; the `if` checks that there is one
first.

## 5. Handle a missing answer

Not every question has an answer in closed form, and that is not an error in
your program — it is an outcome:

```cpp
    const auto hard = px::integrate(px::exp(px::sin(x)), x);
    if (!hard) {
        std::println("no answer: {}", hard.error().message());
        // no answer: no closed form for the integral of %e^sin(x) with respect to x
    }
```

`proxima::cause_of(hard.error())` is `Cause::NoClosedForm`, for code that
wants to branch on why. [Results and failures](../guide/results.md) has the
rest.

## 6. Solve an equation

```cpp
    if (const auto roots = px::solve(eq(f, 0), x)) {
        for (const px::Expr &root : *roots) {
            std::println("root: {}", root);
        }
    }
```

`eq(lhs, rhs)` builds an equation; `==` keeps its ordinary meaning, comparing
two expressions. The roots of this cubic come back exact: `1`,
`(5^(1/2) - 1)/2` and `-(1 + 5^(1/2))/2`.

## 7. Turn expressions into numbers

Numbers need no Maxima. Include `<proxima/numeric.hpp>`:

```cpp
#include <proxima/numeric.hpp>

// ... in main:
    std::println("f(2) = {}", *px::eval_numeric(f, {{x, 2.0}}));   // f(2) = 5

    const px::Compiled fast(f, x);              // prepared once
    double total = 0;
    for (int i = 0; i <= 100; ++i) {
        total += fast(i / 100.0);               // evaluated many times
    }
```

## 8. Add an assumption

Some answers depend on facts about the symbols. Pass them with the question:

```cpp
#include <proxima/assumptions.hpp>

// ... in main:
    const px::Symbol n("n");
    const auto general = px::integrate(pow(x, n), x);
    // fails: Maxima needs to know whether n is -1

    const auto known = px::integrate(pow(x, n), x, px::assuming(gt(n, 0)));
    std::println("{}", *known);                 // x^(1 + n)/(1 + n)
```

The assumption belongs to that one call; nothing is left set up in Maxima.

## Where next

- The [User guide](../guide/expressions.md) goes into each part in depth.
- The how-to guides show common tasks: [typesetting
  results](../how-to/typeset-results.md), [evaluating at many
  points](../how-to/evaluate-many-points.md), [handling
  failures](../how-to/handle-failures.md) and more.
- [`examples/tour.cpp`](../../../examples/tour.cpp) is a runnable walk
  through every public feature.
