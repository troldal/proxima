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
Kind: Integer | Rational | Real | Symbol | Add | Mul | Pow | Function
      | Relation | Opaque
```

Immutable, `shared_ptr<const Node>`, hash cached at construction. Operators
`+ - * /`, unary minus, `pow()`, and a `str()` infix printer.

**Integer type — decided: `int64_t` with an `Opaque` fallback.** No
multiprecision dependency. A value that does not fit becomes an `Opaque` node
carrying its digits: still exact, still printable, still round-trips through
Maxima, just not open to arithmetic on this side. `30!` is the ordinary case,
and an integration test confirms it satisfies `is(<printed> = 30!)` in Maxima.
Widening later means changing the `mx::Integer` alias and the two places that
check for overflow.

The same escape hatch covers the one rational that cannot be normalised:
negating `INT64_MIN` would overflow, so `rational(INT64_MIN, -1)` becomes
`Opaque` rather than wrapping silently.

**Representation — decided: a tagged struct**, not `std::variant` and not a
class hierarchy. No vtable and no visitor machinery, so reading a tree — the
thing this library does constantly — is a switch on an enum. It wastes a few
bytes per node on fields the kind does not use, which is a deliberate trade
against recursive-variant and cross-casting complexity. `Node` never appears in
the public API, so the representation can change without breaking anyone.

**Open question 3 — resolved: `operator==` is structural equality returning
`bool`.** `eq(a, b)` builds an equation. This follows SymPy rather than GiNaC,
and for SymPy's reason: an `==` returning something other than `bool` silently
breaks `std::find`, `std::unordered_map`, and every test assertion. Equations
are rare enough to name explicitly; container lookups are not. Tests exercise
`Expr` in `unordered_set`, `unordered_map` and `std::find` to keep that honest.

Structural means what it says: nothing is simplified and nothing consults
Maxima, so `(x+1)^2` and `x^2+2x+1` are different expressions, and until the
normaliser (step 10) orders operands, so are `x+1` and `1+x`.

**Relations are a typed node**, not `Function("=", …)`, because `solve` has to
destructure them and step 11 should not be comparing head strings to `"="`.
Maxima's spelling of inequality is `#`, not `!=`.

Construction applies only two structural rules: a sum or product of one operand
is that operand, of none is the identity. Exact division of exact integers gives
a `Rational`, and negating a literal gives a literal — both so that the type is
usable before the normaliser exists. Everything else is step 10.

- *Compiles:* yes.
- *Verify:* construction, reduction, equality and hashing tests, no kernel. Plus
  an integration test that each printed expression is **valid Maxima meaning the
  same thing**, using cases where dropping the parentheses would still parse but
  quietly change the value: `-3^2` is `-9`, `3/2^2` is `3/4`, `x^2^3` is `x^8`,
  `x+1*y` is `x+y`, and `x*-2` is not valid Maxima at all.

### Step 9. Translation, both directions

The outbound half already existed: `Expr::str()` (`src/core/printer.cpp`) emits
Maxima infix, and step 8 validated it against live Maxima on cases where
dropping a parenthesis still parses but changes the value. Step 9 is the inbound
half, `src/wire/from_maxima.cpp`.

**Maxima inverts symbol case.** `x` is stored as `$X`, but `X` as `|$x|` and
`xY` as `|$xY|`: a uniformly-cased name is case-inverted, a mixed-case one is
bar-quoted and left alone. Decoding is the same inversion applied again, which
makes it its own inverse. Getting this wrong would silently rename every
variable in the library — and would look perfectly correct for all-lowercase
names, which is most test data. An integration test round-trips `x`, `X`, `xY`,
`alpha` and `x_1` through a live kernel.

**Head spellings.** `%SIN` and `$F` differ only in sigil — `%` for Maxima's own
operators and nouns, `$` for user names — and neither sigil is part of the name.
A *symbol* keeps any `%`, because the constant `%pi` really is called that and
arrives as `$%PI`.

Three heads need an explicit table, for two different reasons:

- `%DERIVATIVE` displays as `'diff`. Printing `derivative(f(x), x)` would be an
  undefined function rather than a derivative, and dropping the quote would ask
  Maxima to evaluate it.
- `MABS`, `MFACTORIAL`, `MNOT` carry no sigil at all, so the general rule would
  produce `mabs`. Only heads with a `name(args...)` spelling can go in this
  table; Maxima's infix operators (MAND, MOR, MNCTIMES) have none, and are noted
  at the fallback.

`MMINUS` and `MQUOTIENT` are handled structurally rather than by name, since
they map onto negation and division.

**Lists are the one head the printer knows by name.** `MLIST` becomes
`Function("list", …)`, and the printer renders that head as `[a, b]` because
Maxima has no textual `list(...)` constructor — the bracket syntax is the only
spelling. Everything else, matrices and derivatives included, is an ordinary
application.

**Bigfloats keep their value, not their type.** A bigfloat is
`mantissa * 2^(exponent - bits(mantissa))`; with no arbitrary-precision float to
map onto, it becomes the exact rational it equals, as `Opaque` text. Nothing is
rounded, and Maxima agrees the two are equal — but sending it back gives a
rational rather than a bfloat. Verified with `is(equal(…, bfloat(%pi)))`.

- *Verify:* unit tests over recorded forms, plus the strongest check in the
  suite — for every recorded expression: evaluate, read the internal form, map
  it, print it back as Maxima source, evaluate *that*, and require the two
  internal forms to match.

  That comparison ignores **simplification flags**. A head records which
  simplifiers have touched the term — `(MEXPT SIMP RATSIMP)` rather than
  `(MEXPT SIMP)` — which is bookkeeping about how a value was reached, not part
  of the value. Maxima's own `integrate` leaves `RATSIMP` behind where
  re-reading the same expression from source does not, so requiring the flags to
  match would fail on a correct round trip. The test strips them by walking the
  s-expression directly rather than going through `fromMaxima`, so it is not
  comparing the mapping with itself.

## Phase 3 — Public API

### Step 10. Normalizer

Applied at construction, in `Expr::add`, `Expr::mul` and `Expr::pow`, so every
expression in existence is canonical and equality never has to re-derive it.

Rules: nested sums and products spliced in; numeric operands folded into one;
identities dropped (`x + 0`, `x * 1`), with a zero factor absorbing the whole
product; `x^1`, `x^0` and `1^n` collapsed; operands ordered.

**Ordering is structural, not by hash.** The original sketch said "sort by
hash", but `std::hash<std::string>` differs between standard library
implementations, which would make canonical form — and therefore printed output
and every test expectation — vary by platform. The order is instead: numbers,
then symbols alphabetically, then compounds. That is also the order Maxima's own
internal representation uses (`x + 1` arrives as `((MPLUS SIMP) 1 $X)`), so
normalisation is a no-op on anything mapped back from Maxima rather than a
reshuffle that obscures diffs.

**Folding refuses to wrap.** Exact arithmetic that would overflow `mx::Integer`
is abandoned and the terms stay unfolded — correct, if less tidy, and far better
than a silently wrong number. The accumulator reduces after every step, which is
what keeps a long sum of fractions from overflowing on the denominators alone.
Inexactness is contagious, as in Maxima: one float makes the whole constant a
float. Exact and inexact are never folded together into equality, so `2` and
`2.0` stay distinguishable.

**What it deliberately does not do**, and the tests say so explicitly: collect
like terms (`x - x` stays `x - x`), expand, factor, or combine powers. That is
algebra, and algebra belongs to Maxima. Doing half of it here would reintroduce
exactly the two-canonicalisers problem that dropping SymEngine avoided.

One consequence worth knowing: `str()` shows canonical order, not the order an
expression was written in. `x + 1` prints as `1 + x`. The printer makes one
display-only concession — a *negative* leading constant is moved to the end, so
`x - 1` reads as written rather than as `-1 + x`; a positive one stays, since
`1 - x` already reads better than `-x + 1`.

- *Verify:* `x + 1` and `1 + x` are equal and hash equally; associativity and
  commutativity hold structurally; `x*1` collapses to `x`; overflow leaves terms
  unfolded rather than wrapping. And the full Maxima round-trip from step 9
  still passes, now with every mapped expression being normalised on the way
  through — evidence that canonical form stays valid Maxima.

### Step 11. Operations and shared kernel

`include/mx/ops.hpp` and `include/mx/functions.hpp`. A lazily-started
`sharedKernel()` is the default last argument of every operation, so the common
case needs no ceremony and an explicit kernel is still available.

**The failure split, as implemented.** Three outcomes, not two:

| | |
|---|---|
| `mx::KernelError` (thrown) | the conversation broke down — kernel died, nothing answered in time |
| `mx::MaximaError` (thrown) | Maxima objected to an operation that has no ordinary way to fail: `diff`, `expand`, `factor`, `simplify`, `subst` |
| `mx::Failure` (returned) | an ordinary mathematical outcome: no closed form, no solution, unparseable source |

`std::expected` is used only where failure is an answer. Making `diff` return one
would force `.value()` on every call for a case that means the caller made a
mistake.

**Maxima does not report "I cannot do this" as an error**, which a probe was
needed to discover and which shapes two of these signatures:

- `integrate(exp(sin(x)), x)` succeeds, returning the integral *unevaluated* as
  `((%INTEGRATE SIMP) …)`. That noun form is the failure signal.
- `solve` returns something that is not a solution: `[x = sin(x)]` for
  `sin(x) = x`, or `[0 = x^5-x-1]` for the quintic. Both are rejected here — an
  equation still mentioning the unknown on both sides, or one never rearranged
  at all — so a successful `solve` really is a solution. An empty result means
  no solutions, which is itself an answer.

**`solve` comes in two forms.** `solve(equation, unknown)` returns the values
for one unknown; `solve(equations, unknowns)` solves a system and returns, per
solution, a value for each unknown *in the order the caller asked for them*.
The single form delegates to the system form, so the rules deciding what counts
as a solution live in one place.

Two things measurement settled here. Maxima **flattens** the result when there
is one unknown — `solve([x^2=1], [x])` gives `[x = -1, x = 1]`, not
`[[x = -1], [x = 1]]` — so the shape is detected from the reply rather than
assumed from the number of unknowns. And although Maxima does answer in the
order the unknowns were given, values are matched back **by name** anyway; that
costs nothing and turns a silent mis-pairing into a detectable missing value.

An underdetermined system solves parametrically, with free parameters appearing
as `%r1`, `%r2`. Those are values like any other and are not among the unknowns,
so they are not grounds for rejection — but a caller wanting only fully
determined solutions has to look for them.

**`parse` is a free function, not `Expr::parse`.** `Expr` belongs to a layer
that knows nothing about the kernel, and parsing needs one. It also only parses:
`parse("a: 7")` yields the assignment as a term and does not perform it.

**A construction rule this step forced.** `x^3/3` was building `x^3 * 3^-1`
while Maxima returns `(1/3)*x^3` — the same value spelled two ways, never
comparing equal. `operator/` now multiplies by the reciprocal when the divisor
is a number, which keeps exact division exact and puts results in the same shape
Maxima uses. Dividing by anything else still becomes a negative power, as it is
in Maxima.

`functions.hpp` supplies `sin`, `cos`, `log`, `abs` and friends as
uninterpreted applications, plus `exp` and `sqrt` built as `%e^x` and `x^(1/2)`
— because that is what they are inside Maxima too, so the representations stay
in step. Nothing is evaluated locally: `sin(0)` stays `sin(0)` until Maxima is
asked.

- *Verify:* the demo is now pure C++ with no Maxima syntax and no strings
  standing in for expressions. Tests cover each operation, both failure
  mechanisms, and chaining — `expand(factor(diff(...)))` — which is the real
  claim: what comes back is an expression, not text.

### Step 12. `Context` / assumptions

`mx::Context` opens a fresh Maxima context on construction and kills it on
destruction. Maxima's own contexts do the work, so this really is a scope rather
than a best-effort undo: `killcontext` discards the assumptions *and* the
declarations, which forgetting each assumption individually would not.

Built on `supcontext(child, parent)` rather than `newcontext`, so contexts nest
— `newcontext` would parent the new scope on `initial` and lose the enclosing
one's facts.

A contradictory assumption throws `mx::MaximaError`. Maxima detects it, and
carrying on with an inconsistent set of facts would make every later result in
the scope meaningless. A redundant one is accepted quietly.

**Deviation from the sketch: no thread-local current context.** The plan called
for one, but it would be a mirror of state that already exists — Maxima's own
`context` variable is the single source of truth, and operations see it without
being told. A C++-side copy could only go out of step with it.

#### The hazard this step really addresses

A probe turned up something worse than a missing feature. Maxima *interrogates
the user* when it lacks a fact: `integrate(x^n, x)` asks "Is n equal to -1?" by
printing a prompt and reading a line from standard input. Over a pipe that is
fatal twice over — the read blocks until the timeout, and then Maxima consumes
the **next request** as the answer, leaving every subsequent reply attached to
the wrong question. A single ambiguous integral would silently corrupt an entire
session.

Overriding Maxima's `retrieve`, the one function all prompting goes through,
turns a question into an ordinary error. `errcatch` then reports it as a
`Failure` carrying the question text, and the session stays synchronised. The
message names the missing fact, so it says exactly what to put in a `Context`:

    int x^n          = no result: this computation needs an assumption that was
                       not supplied. Maxima asked: Is n equal to -1?
      assuming n > 0 = x^(1 + n)*(1 + n)^(-1)

This belongs to the protocol, not to contexts, and is installed in the startup
helper beside the framing. It is the second time the prompt-driven design of a
terminal CAS has had to be defused for pipe use — the first was the prompt
markers in step 6.

- *Verify:* an assumption changes what Maxima concludes (`sqrt(x^2)` is `abs(x)`
  until `x > 0` is assumed, and `abs(x)` again afterwards); a declaration is
  undone on scope exit; nested scopes inherit; a contradiction throws. And three
  tests on the hazard: the question becomes an error, the message is actionable,
  and — the one that matters most — the *next* several calls still answer their
  own questions.

## Phase 4 — Hardening

### Step 13. Timeouts, restart, thread safety

`Kernel` is now serialised, survives its own death, and cannot be deadlocked by
SBCL's debugger.

**Restart and replay.** The session holds a transport *factory* rather than a
transport, so a dead kernel can be replaced. A call that ends in a `KernelError`
— timeout, crash, protocol breakdown — restarts the kernel, replays the journal,
and then reports: only that call is lost.

The journal is what makes this worth doing. A kernel that came back *working*
but missing the caller's assumptions would answer every later question
confidently and wrongly, with nothing to announce that anything had happened —
worse than an outright failure. `mx::Context` registers its `supcontext`,
`assume` and `declare` statements with `Kernel::remember`, and drops them on
scope exit, so replay reconstructs exactly the scopes that are still live. Order
is preserved, which is what makes nested scopes come back nested.

**`--disable-debugger`.** An unhandled Lisp error otherwise drops SBCL into a
debugger that reads standard input — over a pipe, the same deadlock as the
interactive questions in step 12. With it the process exits instead, which
recovery can undo. It is a *toplevel* option, not a runtime one; placed among
the runtime options SBCL refuses to start at all. `errcatch` is unaffected: it
handles the error long before the debugger would see it.

**Two timeouts, not one.** `Config::startupTimeout` (30s) governs launching and
restoring; `Config::timeout` (2 min) governs one computation. Separating them is
not tidiness — with a single value, asking for a one-second deadline on
integrals would make the kernel unstartable, and worse, the deadline that had
just been exceeded would also govern the restart meant to answer it.
`Kernel::setTimeout` adjusts the per-call deadline on a running kernel.

**A real bug found while testing.** The deadline was only checked when a read
came back empty, so a reply arriving as a slow but unbroken trickle would never
test it and could run indefinitely. It is now checked every time round the loop,
and the wait is clamped to the time remaining so the deadline is honoured to
within one poll rather than overshot by one.

**Deviation: no reader thread.** The plan called for one, to stop the child
blocking on a full pipe buffer. On inspection there is no window for that to
happen: the session reads continuously for the whole of a request, and between
requests Maxima emits only a short prompt. The one case where output could pile
up unread — a timeout, while Maxima keeps computing — ends in the process being
killed anyway. A background thread reading a pipe needs careful shutdown, and on
Windows closing a handle out from under a blocking read is racy; that cost is
not worth paying for a window that does not exist. `receive` already honours a
deadline, so the thread would buy no cancellation either.

- *Verify:* scripted transports drive restart-and-replay with no Maxima at all,
  including that a *forgotten* statement is not replayed. Against a real kernel:
  `quit()` kills it, the next call works, and the assumption is still in force;
  an assumption whose scope ended does *not* come back; and a tightened deadline
  on a genuinely slow computation throws `TimeoutError` while leaving the next
  call working.

### Step 14. Memo cache

An LRU of replies, sized by `Config::cacheEntries` (4096; zero disables). A
round trip costs milliseconds and a hit costs nanoseconds, and symbolic work
asks the same questions repeatedly.

**Keyed on the Maxima source text**, not on `(operation, argument hashes)` as
originally sketched. The two are equivalent here, because the source is rendered
from canonical expressions: two calls that should share an answer produce
identical text, and two that should not cannot. Keying on text is simpler and
also catches repetition the operation layer cannot see — the same subexpression
arriving from two different callers.

#### Invalidation is the part that has to be right

A cache that returns a stale answer is a correctness bug; a cache emptied too
eagerly is merely slower. So the rule is to assume the worst:

- **Any `Kernel::eval` discards the cache.** That entry point can evaluate
  anything, and nothing in the text of `a: 7` marks it as an instruction rather
  than a question.
- **`Kernel::evalPure` is the cached path**, and carries an explicit promise:
  the expression only asks. Every operation in `ops.hpp` satisfies it.
- **Adding or dropping an assumption discards the cache**, through the same
  `remember`/`forget` calls `mx::Context` already made for the replay journal.
  That one matters most: `sqrt(x^2)` is `abs(x)` until `x > 0` is assumed and
  `x` afterwards, so a cache surviving the assumption would keep handing back
  `abs(x)` — confidently, and wrongly. A test asserts exactly that, in both
  directions.

Failures are cached too. "Maxima cannot integrate this" is as stable an answer
as any other, and re-asking costs the same round trip.

**Deviation: no Maxima version stamp in the key.** The plan called for one. It
earns its place only in a cache that outlives the process — an in-memory cache
belongs to one kernel running one Maxima, so the version cannot vary within it.
Adding it would mean an extra startup query and re-recording the scripted
transport tests, for nothing. Anyone persisting this cache must add it.

- *Verify:* the LRU on its own (eviction order, replacement, zero capacity),
  then against a real kernel: a repeated question is a hit; a failure is a hit;
  an assumption entering *and* leaving scope changes the answer; a raw `eval`
  empties the cache; a binding changed through `eval` cannot leave a stale
  answer behind; and the warm call is faster than the cold one.

### Step 15. Numeric evaluation and packaging

**`mx::evalNumeric`** walks the tree, entirely locally. That is the point: once
Maxima has produced a closed form, turning it into numbers is ordinary
arithmetic, and paying a millisecond round trip per point would make plotting it
or integrating it numerically absurd. `asFunction` binds one variable for
repeated use; `isEvaluable` asks without catching.

Named constants are recognised as Maxima spells them (`%pi`, `%e`, `inf`,
`minf`), with an explicit binding winning over them.

The function table is a **closed set**, and an unknown head is an error rather
than a guess — silently returning something plausible for a function that is not
actually implemented would be far worse than refusing. The same applies to the
nodes that have no numeric meaning: a relation, and `Opaque`, which is Maxima
source this library never interpreted and so has nothing that could evaluate it.
`mx::EvalError` names the culprit.

**Packaging.** `install`/`export` with a generated `maxima_cppConfig.cmake`, so

```cmake
find_package(maxima_cpp 0.1 REQUIRED)
target_link_libraries(my_app PRIVATE mx::maxima_cpp)
```

works from an install prefix. `mx/version.hpp` is generated from
`cmake/version.hpp.in`, so the version cannot drift from the one in
`CMakeLists.txt`. `target_include_directories` uses `BUILD_INTERFACE` and
`INSTALL_INTERFACE`, so the same target serves both a `add_subdirectory` consumer
and an installed one.

One thing the install surfaced: doctest was being installed alongside the
library, putting a test framework into the consumer's prefix.
`DOCTEST_NO_INSTALL` fixes it — a reminder that a dependency's install rules run
whether or not they are wanted.

- *Verify:* the evaluator against closed forms Maxima produced, including that a
  definite integral agrees with sampling its own antiderivative — the symbolic
  and numeric halves checked against each other. And packaging verified by
  actually doing it: install to a scratch prefix, then build and run a
  standalone consumer that knows nothing of the source tree, on both Windows and
  Linux.

If tree-walking evaluation ever shows up in a profile, the expression can be
printed and handed to ExprTk or muParser — a one-way conversion at a leaf, which
does not reintroduce the two-canonicalisers problem that shaped this design.

## Status

All sixteen steps are done, on Windows and Linux. See README.md for what the
library does and how to use it; the sections above record why it is built the
way it is, including the decisions that were revised along the way.

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

### Settled

1. ~~**Integer type**~~ — step 8: `int64_t` with an `Opaque` fallback, no
   multiprecision dependency.
2. ~~**POSIX transport**~~ — step 5b: both platforms, one `ITransport`.
3. ~~**`operator==`**~~ — step 8: structural equality returning `bool`, with
   `eq(lhs, rhs)` building equations.
4. ~~**`solve` over a system**~~ — built after step 15, once the gap was
   noticed: `solve(equations, unknowns)`, with the single-unknown form
   delegating to it. See step 11.
5. ~~**Offline `parse()`**~~ — built after step 15: `Expr::parse` is a Pratt
   parser over a subset of Maxima's grammar, needing no kernel. `mx::parse`
   remains for anything outside that subset. See below.
6. ~~**A persistent cache**~~ — built after step 15, via
   `Config::cacheDirectory`. See below; the version stamp turned out to be the
   smaller half of the problem.

### Two parsers, and how they differ

`Expr::parse` is a Pratt parser over a deliberate *subset* of Maxima's grammar:
arithmetic, comparisons, function application, lists, strings. It needs no
kernel. Statements — assignment, definition, quoting, non-commutative
multiplication — are refused, because it parses expressions rather than
programs.

Precedences are Maxima's, including the two that catch people out: `^` is
right-associative (`x^2^3` is `x^(2^3)`), and unary minus binds *looser* than
`^` (`-x^2` is `-(x^2)`).

The difference that matters most is not the grammar, though. **`Expr::parse`
parses; `mx::parse` parses and evaluates.** `Expr::parse("5!")` is
`factorial(5)`; `mx::parse("5!")` is `120`, because Maxima evaluates as it
reads. Writing the agreement test against structure rather than meaning was
wrong for exactly this reason, and the test now compares both sides *through*
Maxima: its reading of the original text against its reading of what the offline
parser printed.

### Genuinely undecided

Nothing outstanding.

### The persistent cache, and what its key has to contain

`Config::cacheDirectory` keeps replies between runs, one file per entry. Off by
default: a library should not start writing files somewhere unasked.

Step 14 noted that persisting would need a **Maxima version stamp**. Building it
showed that to be the smaller half. An in-memory cache can be blunt about
invalidation — throw everything away whenever anything might have changed —
because the thing that changed happened here. A cache shared across processes
and across time cannot, because the change may have happened somewhere else
entirely. So everything an answer depends on has to be *in the key*:

- the **Maxima version**, as originally noted;
- **this library's version**, because the stored value is an s-expression read
  back by a mapping that could change, and the key is source rendered by a
  printer that could change;
- the **assumption state**, which is the one that is unsound to omit.
  `sqrt(x^2)` is `abs(x)` normally and `x` under `assume(x > 0)`. A process that
  cached the second would otherwise hand it to a process that never made the
  assumption — and unlike the in-memory case there is no moment at which that
  second process could be told to discard anything.

The assumption state is taken from the replay journal built in step 13, which
already records exactly the statements that constitute it. That also decides
what happens after a raw `Kernel::eval`: nothing in its text says whether it
changed Maxima's state, so the journal can no longer be trusted to describe the
session, and persistence switches itself off for that kernel. `mx::Context` uses
`Kernel::evalTracked` instead, which promises the change *is* in the journal.

Entries are written to a temporary and renamed into place, so a reader never
sees a half-written file and two writers race only to produce identical content.
The key is hashed to name the file and stored inside it as well, so a collision
is detected rather than silently answered wrongly. The hash is FNV-1a rather
than `std::hash`, which varies between standard libraries — a cache on disk
outlives the build that wrote it.

`Kernel::cacheStats().persistentHits` counts answers that came from disk.
Without it the tests could not tell a disk hit from a fresh computation, since
both end up in the in-memory cache.

### Decided, but not built

These are known gaps rather than open questions — the approach is settled, the
work simply is not done. Listed here because a reader scanning this section
should not have to reconstruct them from the step narratives.

7. **Numeric evaluation walks the tree per call.** Fine until it appears in a
   profile, at which point the expression can be printed and handed to ExprTk or
   muParser — a one-way conversion at a leaf, which does not reintroduce the
   two-canonicalisers problem that shaped this design.

8. **`mx::Integer` is 64-bit.** Values beyond it survive exactly, as `Opaque`
   text, but arithmetic on them has to go through Maxima. Widening means
   changing the alias and the two overflow checks that guard it.
