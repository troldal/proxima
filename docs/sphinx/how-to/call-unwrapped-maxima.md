# Call a Maxima function Proxima does not wrap

**Task:** use one of Maxima's thousands of functions that has no C++ wrapper
in `<proxima/ops.hpp>`.

Ask the kernel directly. A `Query` is a question, as text or as an
expression, and the answer comes back as an `Expr`:

```cpp
#include <proxima/kernel.hpp>

namespace px = proxima;

px::Kernel &kernel = px::shared_kernel();

kernel.ask(px::Query::text("gcd(12, 18)"));                           // 6
kernel.ask(px::Query::form(px::Expr::function("gcd", {12, 18})));     // 6, built as an expression
```

Building the question as an expression is the safer of the two: it reaches
Maxima as structure, so nothing in it can be misread, and any `Expr` you
already have can go into it. Text is handy for a one-off.

A query is answered like any operation: cached, and asked under assumptions
if you give them:

```cpp
const px::Symbol x("x");
kernel.ask(px::Query::form(px::Expr::function("abs", {x})),
           px::assuming(gt(x, 0)));                                   // x
```

## Statements

To *change* Maxima — define a function, load a package — use `tell` with a
`Statement`:

```cpp
kernel.tell(px::Statement::text("load(\"distrib\")"));
kernel.ask(px::Query::text("cdf_normal(0, 0, 1)"));                  // 1/2
```

A statement empties the kernel's cache and stops [persistence
between runs](cache-between-runs.md), because nothing says what it changed.
Prefer queries, and give such a kernel its own `Kernel` object rather than
changing the shared one.
