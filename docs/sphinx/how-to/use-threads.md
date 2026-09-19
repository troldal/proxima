# Use Proxima from several threads

**Task:** compute symbolically on several threads at once.

Everything except the kernel is a pure function of its arguments, so
expressions, rendering, traversal and numeric evaluation need no care at all:
share `Expr`s and `Compiled`s between threads freely.

A `Kernel` is safe to share too, but it is one Maxima process answering one
question at a time, so threads sharing it take turns. For work that runs in
parallel, give each thread a kernel of its own and pass it to the operations:

```cpp
#include <proxima/kernel.hpp>
#include <proxima/ops.hpp>

#include <thread>
#include <vector>

namespace px = proxima;

const px::Symbol x("x");
const std::vector<px::Expr> integrands{px::sin(x), px::cos(x), px::exp(x), 1 / x};
std::vector<px::Expr> results(integrands.size());

std::vector<std::jthread> workers;
for (std::size_t t = 0; t < 2; ++t) {
    workers.emplace_back([&, t] {
        px::Kernel kernel;                                  // one Maxima per thread
        for (std::size_t i = t; i < integrands.size(); i += 2) {
            results[i] = *px::integrate(integrands[i], x, kernel);
        }
    });
}
workers.clear();                                            // joins
```

A `Kernel&` converts to the operation's `Env` parameter; so does a `Kernel`
together with assumptions, as `{px::assuming(gt(x, 0)), kernel}`.

Each kernel starts its own Maxima, which takes a moment, so reuse a kernel for
many questions rather than making one per question. Without a kernel argument,
operations use `shared_kernel()`, the process-wide one.
