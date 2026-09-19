# Pure functions and effects

Everything that reads, builds, rewrites, renders or evaluates an expression
is a pure function of its arguments: no I/O, no Maxima, and no state beyond
constants. The effects live in two headers, and `Kernel` is the only type
that holds state.

| Pure — no kernel, no state | Effects — talks to Maxima |
|---|---|
| `expr.hpp`, `integer.hpp`, `symbol.hpp`, `functions.hpp` | `kernel.hpp`: `Kernel`, a Maxima process and its caches, and `shared_kernel()` |
| `assumptions.hpp`: `Assumptions`, `assuming`, `declaring` | `ops.hpp`: every operation |
| `traverse.hpp`: `nodes`, `fold`, `rewrite`, `transform`, `replace` | |
| `numeric.hpp`: `eval_numeric`, `compile`, `Compiled`, `Bindings` | |
| `render.hpp`, `tex.hpp`, `mathml.hpp` | |
| `result.hpp`, `errors.hpp`, `config.hpp`, `version.hpp` | |

The process-wide state is exactly one thing: `shared_kernel()`, started on
first use. (`Compiled` keeps a thread-local scratch stack for its evaluation,
which no caller can observe.) Assumptions are not state anywhere: they are
values, passed with each question. This is a promise, not an accident: it is
what lets a pure result be cached, shared between threads, and composed
without asking what it might have changed.

The source tree follows the same line: `src/expr`, `src/parse`, `src/render`
and `src/numeric` include nothing from `src/kernel`, `src/wire` or
`src/transport`, and CI checks that they do not. [Architecture](architecture.md)
draws the layers.
