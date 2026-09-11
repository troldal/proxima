# PLAN.md — from prototype to a Maxima-backed C++ symbolic library

## Goal

Turn the current single-file prototype into a small, usable C++ library whose
public interface is pure C++ — no Maxima syntax, no strings in the type system,
no backend headers leaking — while Maxima does all the symbolic mathematics in
a persistent child process.

## Constraints

1. **Maxima only.** SymEngine is removed entirely. Maxima is the single source
   of mathematical truth.
2. **No third-party parsers.** The s-expression reader is hand-written; infix
   parsing is delegated to Maxima itself.
3. **Every step compiles.** Each numbered step below is intended to be one
   commit that builds and passes its tests, so the history stays bisectable.

---

## Architecture

```
 Layer 5  Public API          mx::Expr, mx::Symbol, mx::Context
          value semantics, operators, free functions. No <windows.h>,
          no Maxima types in these headers.
              |
 Layer 4  Operation catalog   integrate/solve/limit/factor/simplify
          dispatch + memo cache
              |
 Layer 3  Term translation    Expr -> Maxima source  (printer)
                              Maxima -> Expr         (s-expr reader)
              |
 Layer 2  Kernel session      framing, errcatch, timeouts, state journal,
          crash detection + transparent restart & replay
              |
 Layer 1  Transport (iface)   ITransport: send / receive / alive / kill
          impls: SbclPipeTransport (Win32), FakeTransport (tests)
```

Dependencies point downward only. Layers 1–2 sit behind a pimpl so that
platform headers never reach the public API.

### Key decisions and why

**The library owns a C++ AST; Maxima is an oracle.**
Not a string wrapper, and not a remote-handle proxy. An owned, immutable tree
gives stable hashes (so results can be cached), local inspection without a round
trip, and — critically — expressions that survive a kernel restart after a
timeout. A handle-based design would lose everything when the kernel dies.

The node set stays small because of two escape hatches:

- `Function(name, args...)` — an uninterpreted application, so `bessel_j`,
  `%gamma` or a user-defined `f` round-trip verbatim with no new C++ type.
- `Opaque(raw)` — last resort for non-applications (matrices, intervals,
  integration constants), so a Maxima result is *never* unrepresentable.

**The wire format is Maxima's internal s-expressions, not its display output.**
`(%oN)` infix text is ambiguous, line-wraps, and is a display format Maxima is
free to change. The internal form is unambiguous (no precedence to re-derive),
preserves exact rationals and bignums, and parses with a tokenizer plus a stack.
See step 6.

**One canonicalizer.** This is the main reason SymEngine is dropped rather than
retained as a local fast path. SymEngine auto-canonicalizes on construction;
Maxima simplifies by its own assumption-aware rules. With both live, the same
expression has two canonical forms depending on which path produced it — which
breaks structural equality and makes cache keys miss unpredictably. Maxima owns
simplification; the local normalizer (step 10) only stabilizes hashing.

**Subprocess, not embedding.** Maxima is GPL. Running it as a separate process
communicating over pipes keeps the licenses separate. Embedding it in-process
(it can be built on ECL as a shared library) would pull GPL into the address
space of every consumer. The process boundary is a licensing requirement, not
just a convenience — do not "optimise" it away.

**No third-party expression library for parsing.** muParser, ExprTk and TinyExpr
are numerical evaluators: they compile to an evaluation plan and return a
`double`, with no walkable AST. Lepton does expose an AST but, like the others,
stores constants as `double` — so `1/3` becomes `0.333...` at the front door.
For a CAS that is silent data loss. Parser toolkits (PEGTL, lexy) would work but
are larger than the problem. Instead: hand-write the s-expr reader, and delegate
infix parsing to Maxima's own `parse_string`.

---

## Verified environment (Maxima 5.50.0, Windows)

```
C:\maxima-5.50.0\bin\sbcl.exe
C:\maxima-5.50.0\bin\sbcl.core
C:\maxima-5.50.0\lib\maxima\<version-tag>\binary-sbcl\maxima.core
C:\maxima-5.50.0\share\maxima\<version-tag>\
```

The Maxima core lives under `lib/maxima/<tag>/binary-sbcl/`, **not** beside
`sbcl.exe`. A glob on that known shape replaces the prototype's full recursive
scan, which currently walks `gnuplot`, `vtk`, `clisp-2.49` and `doc` and could
latch onto the wrong `sbcl.exe`.

---

## Target layout

```
include/mx/          expr.hpp  symbol.hpp  ops.hpp  context.hpp
                     config.hpp  errors.hpp  numeric.hpp
src/core/            node.cpp  normalize.cpp  printer.cpp  eval.cpp
src/wire/            sexpr.hpp/.cpp        (generic s-expr tree + reader)
                     to_maxima.cpp         (Expr -> Maxima source)
                     from_maxima.cpp       (SExpr -> Expr)
src/kernel/          kernel.cpp  discovery.cpp  journal.cpp  cache.cpp
src/transport/       itransport.hpp  sbcl_win32.cpp  fake.cpp
examples/demo.cpp
tests/
```

Namespace `mx`, library target `maxima_cpp`. Rename to taste.

---

## Phase 0 — Clean, small baseline

### Step 0. Repository hygiene

`git init`; add a `.gitignore` covering `build/`, `cmake-build-*/` and
`.idea/workspace.xml`. Commit the prototype as-is so the starting point is
recoverable.

### Step 1. Remove SymEngine

Delete the CPM / GMP / Boost / `INTEGER_CLASS` / `WITH_SYMENGINE_RCP` blocks
from `CMakeLists.txt` — that is essentially all of its platform-specific
complexity. Strip every SymEngine include and call from `main.cpp`; the demo
becomes "start a session, evaluate a few statements, print the raw text".

- *Compiles:* yes.
- *Verify:* `integrate(x^2*sin(x),x)` still prints its infix text.
- *Payoff:* `CMakeLists.txt` drops from ~55 lines to ~10; the build no longer
  needs vcpkg or Strawberry Perl.

### Step 2. Library target and directory structure

Create the layout above. `main.cpp` becomes `examples/demo.cpp`. Move
`MaximaSession` verbatim into `src/kernel/kernel.cpp` with an internal header.
Add a `maxima_cpp` library target that the demo links. Public surface for now:

```cpp
namespace mx { class Kernel { public: std::string evalRaw(std::string_view); }; }
```

- *Compiles:* yes, behaviour unchanged.
- *Verify:* demo output identical to step 1.

### Step 3. Test harness

`enable_testing()`, a `tests/` target, CTest, one trivial passing test. Pick the
framework now so later steps have somewhere to put tests: doctest via the
already-vendored CPM (single header, fast compiles), or plain functions plus
`assert` for zero dependencies.

- *Verify:* `ctest` runs green.

---

## Phase 1 — Make the wire trustworthy

### Step 4. Transport interface

Extract from `Kernel`:

```cpp
struct ITransport {
    virtual ~ITransport() = default;
    virtual void send(std::string_view) = 0;
    virtual std::string receive(std::chrono::milliseconds) = 0;
    virtual bool alive() const = 0;
    virtual void kill() = 0;
};
```

`SbclPipeTransport` holds all the current Win32 code. `FakeTransport` replays a
scripted list of responses. `Kernel` takes a `unique_ptr<ITransport>` through a
pimpl, so `<windows.h>` disappears from every header.

- *Compiles:* yes.
- *Verify:* a `FakeTransport` test that runs with no Maxima installed.
- *Why here:* this is the step that makes every later step testable in CI.

### Step 5. Real discovery

Replace `findFile`'s recursive walk with an ordered strategy: explicit
`Config::maximaRoot` -> `MAXIMA_ROOT` env -> known install paths / registry ->
`PATH`. Within the chosen root, look for `bin/sbcl.exe` and glob
`lib/maxima/*/binary-sbcl/maxima.core` directly.

Set `MAXIMA_PREFIX` and `SBCL_HOME` in the child environment, and pass
`--dynamic-space-size 2000` on 64-bit builds, matching what upstream's
`maxima.bat` does.

> **Correction (measured during step 5).** An earlier draft of this plan claimed
> `MAXIMA_PREFIX` was required for share packages to load. That is not so on a
> standard install: the core has its paths compiled in, and `load(abs_integrate)`
> plus `integrate(abs(x),x)` both succeed with the variable unset. It is still
> worth setting, but for a narrower reason — a relocated or portable
> installation whose baked-in prefix no longer exists.

The measurement did turn up a real hazard: Maxima loads `maxima-init.mac` from
`$MAXIMA_USERDIR` (default `~/maxima`) at startup, and wxMaxima creates that file
as a matter of course. Anything in it silently changes this library's results on
one machine and not another. A library must compute the same answer everywhere,
so `Config::loadUserInit` defaults to false and the user directory is pointed at
a controlled location. Setting it true opts back in.

- *Verify:* startup is visibly faster; a bogus root produces a clear error
  instead of a 30-second scan; `maxima_userdir` read back from a live session
  reflects the configured value, which makes the whole environment path
  observable end to end.

**Explicit configuration is authoritative.** If `Config::maximaRoot` is set but
unusable, discovery throws rather than searching on. Falling through would
silently run a different installation than the caller asked for, turning a
mistyped path into surprising results instead of a diagnosable error. (Caught by
a test during step 5, which the first implementation failed.)

### Step 6. Framed protocol with correlation IDs

At startup, install a Lisp helper in the image:

```lisp
(defmfun $cppsend (id x)
  (let ((*print-circle* nil) (*print-pretty* nil) (*print-readably* nil))
    (format t "~&@@B~a@@~s@@E~a@@~%" id x id)))
```

and set `display2d:false$` and `nolabels:true$` — the latter stops unbounded
`%i`/`%o` label accumulation. Every request then becomes:

```maxima
cppsend(<id>, errcatch(ratdisrep(<expr>)))$
```

- `errcatch` returns `[]` on failure and `[result]` on success: a structured
  error channel instead of scraping error prose, and Maxima never drops into an
  error prompt that desyncs the pipe.
- `ratdisrep` prevents CRE (`MRAT`) forms coming back.
- The correlation ID makes a desync *detectable* rather than silently returning
  the previous answer.

**As implemented**, the frame carries three fields rather than one, because
measurement showed `errcatch` alone was not enough:

```
@@B<id>@@<ok>@@S<id>@@<value>@@S<id>@@<reason>@@E<id>@@
```

- `errcatch` still prints its message *outside* the frame, where it would be
  indistinguishable from a prompt. `errormsg:false$` silences that, and the
  helper renders the message into the frame by calling `errormsg()` with
  `*standard-output*` bound to a string.
- The Maxima variable `error` is **not** usable as the failure signal: it holds
  a format string plus arguments rather than rendered text, and it is stale on
  success (it still names the *previous* failure). Only the shape of the
  `errcatch` list says whether this request failed.
- Assigning to `error` is itself an error — it is a protected symbol.

**The prompt markers are gone entirely.** With explicit frames there is nothing
for them to do: everything before a frame's opening delimiter is banner text,
prompts, or a stale frame, and all of it is discarded. The handshake is now a
framed probe — reading until *its* frame arrives is what synchronises the
stream.

`mx::Kernel::evalRaw(statement)` becomes `eval(expression)` returning
`mx::Reply { ok, value, reason }`. Note the argument change: an expression with
no terminator, since it is substituted into a wrapper that supplies its own.

- *Compiles:* yes.
- *Verify:* the demo prints internal form, e.g.
  `((MTIMES SIMP) 2 $X ((%SIN SIMP) $X))`, and `1/3 + 2/5` comes back as
  `((RAT SIMP) 11 15)` rather than `0.7333…`.
- *Golden data:* `tests/golden/internal_forms.tsv`, 39 recorded cases,
  regenerated by `tests/golden/record.sh`. Steps 7 and 9 are written against it.

**What the golden data settles for later steps:**

- `30!` is `265252859812191058636308480000000` — bignums exceed `int64` in
  ordinary use, so step 8's integer decision is real, not theoretical.
- `true`/`false` are Lisp `T`/`NIL`, not `$TRUE`/`$FALSE`.
- Bigfloats are their own node: `((BIGFLOAT SIMP 56) 56593902016227522 2)`.
- Matrices and derivatives need no special case — `(($MATRIX SIMP) …)` and
  `((%DERIVATIVE SIMP) …)` are ordinary function heads, which is exactly what
  the `Function` escape hatch is for.
- Heads come in two spellings: `%SIN` for built-ins, `$F` for user functions.
- Flags beyond `SIMP` occur (`((MEXPT SIMP RATSIMP) $X 2)`), so the reader must
  treat the head as a list whose first element is the operator, not a pair.

---

## Phase 2 — The term model

### Step 7. S-expression reader

`SExpr` as a variant (`Integer`, `Real`, `Symbol`, `String`, `List`) plus a
tokeniser and a stack-based reader. Pure, no I/O, no Maxima knowledge.

**Deviation from the original sketch.** The plan said `Kernel` would gain
`SExpr eval(...)`. It did not: `SExpr` stays internal, in `src/wire/`, and the
public API is unchanged. Exposing it would publish a type that steps 9 and 11
then retract once `mx::Expr` arrives — a breaking change for no gain, since
`Expr`'s `Opaque` node already covers the escape-hatch case `SExpr` would have
served. The reader is exercised instead by tests on three fronts: string
literals, the recorded golden file, and live Maxima replies.

**Integers are stored as digits, not as an integer type.** `30!` is
`265252859812191058636308480000000`, so the reader cannot pick a fixed width
without losing values. Keeping the text makes the reader lossless and leaves the
numeric-representation choice to step 8, which is where it belongs; `asInt64()`
returns `nullopt` rather than wrapping when the value does not fit.

**Deliberate rejections.** Dotted pairs throw rather than being read as a symbol
named `.` — Maxima's term representation is proper lists throughout, so one
appearing means something unmodelled arrived, and quietly mis-reading it would
corrupt the tree. Input containing two expressions throws too, since that means
the frame was mis-split. Nesting is capped at `kMaxSExprDepth` so a pathological
reply cannot overflow the stack when the tree is destroyed.

Lisp's alternative exponent markers (`1.5d0`, `1.5s0`) are accepted. Maxima sets
`*read-default-float-format*` to double-float so `e` is what arrives in
practice, but the others are legal Common Lisp and would otherwise fail to parse
mystifyingly.

- *Verify:* unit tests from string literals; every one of the 39 recorded golden
  forms parses **and** re-renders to something that reads back identically,
  which catches a reader that accepts input but silently loses part of it; and
  the same round-trip against live replies, which is what catches the recording
  going stale after a Maxima upgrade.

### Step 8. The `Expr` value type

```
Kind: Integer | Rational | Real | Symbol | Add | Mul | Pow | Function | Opaque
```

Immutable, `shared_ptr<const Node>`, hash cached at construction. Operators
`+ - * /`, unary minus, `pow()`, and a `str()` infix printer.

**Open decision — integer type.** To keep this step dependency-free, define
`using Integer = std::int64_t;` in one header and overflow-check at
construction, falling back to `Opaque` holding the digit string. One typedef to
swap for Boost.Multiprecision or GMP later. Flagged because Maxima *will* hand
back bignums as soon as anything interesting is factored.

- *Compiles:* yes; nothing consumes it yet.
- *Verify:* construction and hashing tests, no kernel.

### Step 9. Translation, both directions

- `to_maxima.cpp` — `Expr` -> Maxima source with correct precedence and
  parenthesisation.
- `from_maxima.cpp` — `SExpr` -> `Expr`: `MPLUS`->Add, `MTIMES`->Mul,
  `MEXPT`->Pow, `RAT`->Rational, `%SIN`->`Function("sin")`, `$X`->`Symbol("x")`,
  `$FOO`->`Function("foo")`, `%E`/`%PI`/`%I`->constants, anything unrecognised
  -> `Function` or `Opaque`.

- *Verify:* round-trip tests against the step 6 golden transcripts — these run
  without Maxima.

---

## Phase 3 — Public API

### Step 10. Normalizer

Flatten nested `Add`/`Mul`, fold numeric constants, sort operands by hash, drop
identities. Applied at construction. This exists for **stable hashing and
equality**, not simplification — Maxima owns simplification.

- *Verify:* `x+1` and `1+x` hash equal; `x*1` collapses to `x`.

### Step 11. Operations and shared kernel

```cpp
Expr diff(const Expr&, const Symbol&, unsigned n = 1);
std::expected<Expr, Reason> integrate(const Expr&, const Symbol&);
std::expected<Expr, Reason> limit(const Expr&, const Symbol&, const Expr& to);
std::vector<Solution> solve(std::span<const Equation>, std::span<const Symbol>);
Expr expand(const Expr&), factor(const Expr&), simplify(const Expr&);
```

A lazily-started process-wide `Kernel::shared()`, plus explicit `Kernel`
construction for callers who want control. `Expr::parse()` delegates to Maxima's
`parse_string` — zero parser code, and the accepted syntax cannot drift from the
backend's.

Errors split two ways: infrastructure failures (`mx::KernelError`,
`mx::TimeoutError`) throw; mathematical failure ("no closed form") returns
`std::expected`, because that is an ordinary outcome rather than an exception.
C++23 is already enabled in this project.

- *Verify:* the demo becomes pure C++ — no strings, no Maxima syntax:

```cpp
mx::Symbol x("x");
auto I = mx::integrate(x*x*mx::sin(x), x);
std::cout << I->str() << '\n';
std::cout << mx::expand(mx::diff(*I, x)).str() << '\n';   // recovers x^2*sin(x)
```

### Step 12. `Context` / assumptions

RAII assumption scope (`assume`, `declare`) with a thread-local current context,
replayed into the kernel and contributing to the step 14 cache key.

---

## Phase 4 — Hardening

### Step 13. Timeouts, restart, thread safety

A dedicated reader thread draining into a bounded queue, so the child never
blocks on a full pipe buffer. Per-request deadlines. `--disable-debugger` plus a
`*debugger-hook*` that emits a framed failure instead of blocking forever on
stdin.

A **state journal** on the C++ side records `assume`/`declare`/definitions, so
timeout -> kill -> restart -> replay is transparent. Expressions are local
values and survive untouched — the payoff for choosing an owned AST.
Mutex-serialize `Kernel`; `Expr` is immutable and needs no locking.

### Step 14. Memo cache

LRU keyed on `(operation, argument hashes, context hash)` plus a Maxima version
stamp. Nearly free given immutable hashed expressions, and symbolic workloads
re-ask the same questions constantly.

### Step 15. Numeric evaluation and packaging

A tree-walking evaluator,
`double evalNumeric(const Expr&, const std::map<Symbol,double>&)` — roughly 150
lines, no dependency. Then install/export targets so `find_package(maxima_cpp)`
works, a README, and a version header.

If JIT-speed numeric evaluation is ever needed, print `Expr` to a string and
hand it to ExprTk or muParser. That is a one-way conversion at a leaf, so it
does not reintroduce the dual-canonicalization problem.

---

## Sequencing notes

Two orderings are deliberate:

- **Step 4 before anything substantial.** Once transport is an interface, every
  later step is testable without a Maxima install, which keeps CI honest.
- **Step 6 before steps 7–9.** The reader needs real recorded transcripts to be
  written against, and they must be in s-expression form so they do not have to
  be re-recorded later.

Steps 1–6 take the prototype to a trustworthy wire protocol and are worth doing
in one sitting. Steps 7–11 are the library proper. Steps 12–15 can lag behind
real usage.

## Step 5b — Linux support (done, out of original order)

Taken before step 6 rather than after, because the transport is
protocol-agnostic by construction: it moves bytes and knows nothing of prompts
or markers. A POSIX transport written against the current protocol therefore
keeps working unchanged when step 6 replaces the wire format, and step 6 then
lands on both platforms at once.

**Which Lisp.** On openSUSE: `maxima` plus `maxima-exec-sbcl`. SBCL compiles to
native code where CLISP is a bytecode VM, and in a library where Maxima does all
the mathematics the Lisp's speed *is* the performance. It also keeps one launch
shape across both platforms — `sbcl --core … --eval …` is identical on Windows
and Linux, where CLISP would need `-M image.mem` and `-x form` — and `sbcl`'s
`--disable-debugger` is what step 13 assumes. CLISP's genuine advantage is
bignum arithmetic; it does not apply to symbolic calculus. Note that
`maxima-exec-sbcl` requires an *exact* SBCL version (`sbcl = 2.6.8-1.1`), since
cores are locked to the runtime that dumped them.

**Layout differences, all measured on openSUSE Tumbleweed:**

| | Windows | Linux |
|---|---|---|
| runtime | `<root>/bin/sbcl.exe` | `/usr/bin/sbcl` |
| core | `<root>/lib/maxima/<tag>/binary-sbcl/` | `/usr/lib64/maxima/5.50.0/binary-sbcl/` |
| version tag | `branch_5_50_base_9_gf03405fbf_dirty` | `5.50.0` |
| `SBCL_HOME` | set to `<root>/bin` | **must not be set** |
| `--dynamic-space-size` | set on 64-bit | not set |
| PATH separator | `;` | `:` |
| install root | its own directory | an existing prefix (`/usr`) |

`SBCL_HOME` is the trap. Upstream's `maxima.bat` sets it because the Windows
bundle keeps `sbcl.core` beside `sbcl.exe`; a distribution SBCL has its home
compiled in (`/usr/lib/sbcl` on openSUSE), which is *not* `<root>/bin`.
Overriding it on Linux would break contrib loading rather than fix anything, so
it is `#ifdef`-ed to Windows and a test asserts its absence on Unix.

**What changed:**

- `ChildProcessTransport` is now a pimpl, so `<windows.h>` appears in no header
  at all. Exactly one of `child_process_win32.cpp` / `child_process_posix.cpp`
  is compiled.
- The POSIX transport uses `poll()`, which takes the deadline directly — no
  polling loop and no sleep, unlike the Windows side, where a blocking read on
  an anonymous pipe cannot be abandoned.
- A failed `exec` is reported synchronously through a close-on-exec status pipe,
  matching `CreateProcess`'s behaviour, rather than surfacing later as a child
  that mysteriously exits with 127.
- Environment merging moved to `mergeEnvironment`, shared but **not** uniform:
  case-insensitive name matching on Windows, case-sensitive on POSIX, because
  that is how the two platforms actually compare variable names.
- Discovery gained `lib64`, the platform's PATH separator, `sbcl` vs
  `sbcl.exe`, and Unix prefixes (`/usr/local` before `/usr`, plus `/opt/maxima*`).

`quoteArg` turned out to be unnecessary on POSIX: `execve` takes the array
directly. Step 4's choice to pass **argv as a vector rather than a command-line
string** is what made that free.

`CMakePresets.json` provides `windows`, `linux` and `wsl` presets; the `wsl` one
keeps the build tree on the Linux filesystem, since building under `/mnt/c`
crosses the 9P bridge for every file operation.

Tests: 32 Maxima-free on Windows, 29 on Linux (the difference is the Win32
quoting and environment-block tests), 7 integration on both.

## Open questions

1. **Integer type** (step 8) — `int64_t` with an `Opaque` fallback now, or take
   the Boost.Multiprecision / GMP dependency up front?
2. ~~**POSIX transport**~~ — done, see step 5b above.
3. **`operator==`** — return `Equation` (CAS-idiomatic, ergonomic, but breaks
   `std::find` and generic algorithms) or keep it boolean and require
   `eq(lhs, rhs)` to build equations?
4. **Offline `parse()`** — is there a requirement to construct expressions from
   strings without a running kernel? If so, a hand-written Pratt parser
   (~250 lines) is needed; otherwise delegation to Maxima is sufficient.
