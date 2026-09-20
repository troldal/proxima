# Proxima architecture

How the library is put together: its layers, and how the subsystems work
together. Individual classes are left out. [`docs/design.md`](docs/design.md)
says *why* each piece is the way it is; this page says *where* it is.

## The layers

Proxima has two halves over one shared expression type. The left half runs in
your process and never needs Maxima. The right half asks Maxima, running as a
child process, and brings the answer back as an expression.

```
                      +----------------------------------+
                      |           your program           |
                      +----------------------------------+
                         |                            |
      local: no Maxima   |                            |   asks Maxima
                         v                            v
 +--------------------------------------+  +------------------------------+
 | EXPRESSIONS       include/proxima/   |  | OPERATIONS          ops.hpp  |
 |  Expr, Symbol, Integer, functions    |  |  diff integrate limit solve  |
 |  (sin, pi, ...), traversal, result,  |  |  taylor factor is parse ...  |
 |  Assumptions                         |  |  build a form, ask, read it  |
 +--------------------------------------+  +------------------------------+
 | LOCAL SERVICES          src/*/       |  | KERNEL           kernel.hpp  |
 |  parse/    Expr::parse, offline      |  |  ask(query, assumptions)     |
 |  render/   infix, TeX, MathML, yours |  |  tell(statement)             |
 |           operator<<, std::format    |  |  Env: which kernel, and      |
 |  numeric/  eval_numeric, compile     |  |  under which assumptions     |
 +--------------------------------------+  +------------------------------+
 | CORE                    src/expr/    |  | SESSION         src/kernel/  |
 |  immutable node tree                 |  |  reply caches, memory + disk |
 |  normaliser: one canonical form      |  |  a Maxima context per        |
 |  Integer: int64, or a big integer    |  |  assumption set; protocol;   |
 +--------------------------------------+  |  timeouts and recovery       |
                    ^                      +------------------------------+
                    |                      | WIRE              src/wire/  |
                    +----------------------|  Expr  -> s-expression text  |
                     everything the right  |  text  -> SExpr  -> Expr     |
                     half sends or returns +------------------------------+
                     is an Expr            | TRANSPORT    src/transport/  |
                                           |  bytes over pipes; knows     |
                                           |  nothing about Maxima        |
                                           +------------------------------+
                                                          |  stdin / stdout
                                                          v
                                           +------------------------------+
                                           | CHILD PROCESS                |
                                           |  SBCL running maxima.core,   |
                                           |  plus a small Lisp helper    |
                                           |  installed at startup        |
                                           +------------------------------+
```

Each layer uses only the ones below it. The public headers are the top two
boxes on each side. Everything in `src/` is internal.

## What needs Maxima, and what does not

```
  LOCAL: works with no Maxima installed    KERNEL: needs Maxima
  -------------------------------------    -----------------------------------
  building and combining expressions       diff, integrate, limit, sum, taylor
  exact arithmetic, big integers           solve, ode2, factor, expand, ...
  Expr::parse   (a subset of the syntax)   proxima::parse (all of Maxima's)
  rendering: text, TeX, MathML, custom     is (asking whether a fact holds)
  eval_numeric, compile to a Compiled      anything sent through Kernel::ask
  traversal: nodes, fold, rewrite          Kernel::tell (changing its state)
  Assumptions: built and compared here     ... and applied in Maxima
```

Assumptions are built locally, but they only mean something once a kernel
applies them. The test suite follows the same split: `ctest -LE maxima` runs
everything that needs no Maxima. That includes the whole left side, plus the
session's protocol, which it tests against a scripted fake.

## The life of one question

What happens when you write:

```cpp
proxima::integrate(pow(x, n), x, proxima::assuming(gt(n, 0)))
```

```
 proxima::integrate(f, x, env)                               kernel/ops.cpp
   |  builds the form  integrate(x^n, x)  as an ordinary Expr
   |  env: the shared kernel unless one is given, plus the assumptions
   v
 Kernel::ask(Query::form(form), assumptions)                 kernel/kernel.cpp
   |  turns the assumptions into an Environment: a canonical key, and
   |  the declare(...) / assume(...) statements that establish them
   v
 MaximaSession::eval_pure                                    kernel/session.cpp
   |  cache key = environment key + the request
   |
   |--> in-memory LRU ............ hit? -----------------------------+
   |--> on-disk cache (opt-in) ... hit? -----------------------------+
   |                                                                 |
   |  miss: switch Maxima to the context for these assumptions,      |
   |        creating it if new (up to 16 are kept, least recently    |
   |        used dropped)                                            |
   |  to_maxima(form)  ->  ((%INTEGRATE) ((MEXPT) $X $N) $X)   wire/ |
   v                                                                 |
 one framed exchange over the pipe                  transport/       |
   >>  cppsend(<id>, errcatch(ratdisrep(cppread("..."))))$           |
   <<  @@B<tag>@@status@@S<tag>@@ value @@S<tag>@@ reason @@E<tag>@@   |
       where <tag> is <key>-<id>                                     |
   |                                                                 |
   v                                                                 |
 Reply: a value, or a failure and its Cause --> both caches          |
   |                                                                 |
   |<----------------------------------------------------------------+
   v
 to_result: a failure becomes a Failure with a Cause           kernel/reply.hpp
   v
 SExpr reader  ->  from_maxima  ->  normaliser  ->  Expr        wire/, core/
   v
 result<Expr>, which is fxt::expected<Expr, fxt::failure>
```

Two details of the exchange matter:

- **Nothing you send is read as Maxima syntax.** The expression travels as a
  string literal the wire layer escaped itself. The Lisp helper reads it back
  into structure inside `errcatch`, so a bad expression fails as an ordinary
  error.
- **The reply is Maxima's internal form, not its display output.** It comes
  back as the same kind of s-expression that was sent. No infix text is
  parsed in either direction.

## When something goes wrong

The failures a caller can do something about arrive as values: a `Failure`
whose `Cause` says what happened. `unwrap()` turns one into the matching
exception if you prefer those.

```
 what happened                          how it is caught        Cause
 -------------------------------------  ----------------------  ----------------
 Maxima reports an error                errcatch, into the      MaximaError
                                        frame's reason
 Maxima wants to ask "Is n positive?"   the helper intercepts   NeedsAssumption
                                        the question, and says
                                        so in the frame's status
 the assumptions contradict each other  assume() says so        Inconsistent
 no answer in closed form               read from the result    NoClosedForm,
                                                                NotSolved, ...
```

Kernel failures are thrown instead, after the session has been repaired:

```
 Maxima hangs past the timeout  --+
                                  +-->  recover():
 Maxima dies                    --+       terminate the child process
                                          start a new one, install the helper
                                          forget contexts, cached answers, and
                                          anything tell() had changed
                                        --> only this call is lost; the next
                                            call finds a working kernel
```

## Inside the session

The session is the only part with moving pieces. It holds the conversation —
one request at a time, the caches kept honest across it, and recovery when
Maxima dies — and takes the rest from units beside it that know nothing of
the conversation and are tested without one:

```
 +------------------------------ MaximaSession -------------------------------+
 |  the conversation: send a request, read its frame, cache the answer,       |
 |  switch context, recover                                                   |
 |                                                                            |
 |  ReplyCache            PersistentCache          ContextTable               |
 |  in memory, LRU,       one file per answer;     one Maxima context per     |
 |  bounded by count      the key includes the     distinct Assumptions, LRU  |
 |  and bytes             Maxima and library       of sixteen; the session    |
 |                        versions                 does the switching         |
 |                                                                            |
 |  protocol              launch                   discovery (at startup)     |
 |  Payload, numbered     the SBCL command line,   finds sbcl + maxima.core:  |
 |  frames with a random  the environment, the     Config, MAXIMA_ROOT, then  |
 |  key, the helper Lisp, private user directory   PATH and the usual places  |
 |  reading a frame back                                                      |
 +----------------------------------+-----------------------------------------+
                                    |  ITransport: send bytes, receive bytes
                  +-----------------+-----------------+
                  v                                   v
      ChildProcessTransport                     FakeTransport
      Boost.Process, every platform             scripted replies, so the
                                                tests exercise the protocol
                                                without Maxima
```

- **Threads:** a kernel serves one request at a time, under one lock around
  the pipe, and that includes cache lookups. A second lock guards the caches
  themselves, so `cache_stats()` and `invalidate_cache()` never wait for
  Maxima. For parallel work, use one `Kernel` per thread.
- **Stale answers:** `tell()` or `invalidate_cache()` bumps a generation
  counter, so an answer computed across the change is never cached.
- **Disk cache:** it switches off after `tell()` has changed the kernel's
  state, and on again after a restart discards the change.

## The local side

**Expressions.** An `Expr` is an immutable pointer to a shared node. Every
node is built through the normaliser. Sums and products are sorted, numbers
folded, and identities removed, so equal expressions have equal trees, which
makes hashing and comparison cheap.

**Rendering** happens in three stages. Only the last one is yours to replace:

```
 Expr --> display tree ------------> walk --------------> renderer
          (presentation: 1/3 as a    (decides where        infix  TeX  MathML
           fraction, x^(1/2) as a     brackets go)         or any type you
           root, x - 1 not -1 + x)                         write: a concept,
                                                           no base class
```

**Numbers:**

- `eval_numeric` walks the tree once, for a single value.
- `compile` flattens it to a small stack program. Evaluating that at many
  points costs a loop over instructions, with no tree walk and no lookups.

## Where things live

```
 include/proxima/   the public API: everything a user includes
 src/expr/          the expression model: Expr, the normaliser, Integer,
                    traversal, Assumptions
 src/parse/         the offline parser, Expr::parse
 src/render/        the rendering walk; infix, TeX, MathML; std::format
 src/numeric/       eval_numeric, compile
 src/kernel/        Kernel, the operations, the session and the units beside
                    it: protocol, launch, the context table, caches, discovery
 src/wire/          s-expression reader, and the Expr <-> Maxima mapping
 src/transport/     ITransport, the child process, the scripted fake
 src/util/          UTF-8 conversions, used wherever paths or the
                    environment cross into the operating system
 tests/             doctest; the `maxima` label marks what needs a kernel
 docs/              the guide and the design history; docs/sphinx builds the
                    documentation site, with the API reference
 fuzz/              libFuzzer targets for the two hand-written parsers
 examples/          demo.cpp, and tour.cpp: every public feature, runnable
```

Outside dependencies:

- **Boost.Multiprecision:** big integers, private to `Integer`.
- **Boost.Process:** starting the child process, private to the transport.
- **FXT:** `result<T>` is its `expected`, so FXT's pipeline adaptors work on
  Proxima's results directly.
