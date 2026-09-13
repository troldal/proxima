# TODO — review findings

A full read of the project (all ~10,800 lines: public headers, every source
file, the tests, the build) followed by probes that *ran* the suspicious cases
rather than reasoning about them. Where a claim below is measured, it says so.

**Overall.** The architecture is sound and the discipline is unusually good:
layers only point downward, no public header includes an internal one, the
whole library compiles with zero warnings under
`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
-Wold-style-cast` (checked, all 19 translation units), and 222 tests pass
against a real Maxima. The protocol design — correlation IDs, `errcatch`,
the `retrieve` override, internal s-expressions rather than display text —
is the part most projects like this get wrong, and it is right here.

The problems are at the edges: a handful of real semantic bugs in the
numeric layer, one robustness hole that turns a typo into a two-minute
stall, an API with a couple of silent traps, and performance costs that
are fine today but will not scale. None of it is structural.

Items are ordered by how much they matter, not by file.

## Status since the review

Updated after `39dad91`. Resolved findings are ticked where they stand, with
an *Outcome* note; everything unticked is still open. The review's own text
is left as written, so its measurements stay comparable.

Work since, and what it turned up that the review had not found:

- **Expressions reach Maxima as s-expressions** (`b71ccdf`). Resolves §2's
  read-error stall and closes the injection path with it, plus the §8
  read-error test and three §5 stale comments. Found on the way: `subst` is an
  alias that Maxima's parser resolves and reading a form does not, so a form
  headed `$SUBST` evaluated to itself; and two tests had been passing for the
  wrong reason, provoking errors with `Symbol("5")` that only erred because
  printing turned the symbol into a number.
- **User-supplied renderers** (`666756c`). A type-erased `mx::Renderer<T>` over
  a shared presentation layer, with `str()` reimplemented on top of it.
  Resolves §3's "printer runs the normaliser" and §4's `-1*x` printing. Found:
  `x - (1 + y)` printed as `x - 1 + y`, which re-parses as a different
  expression.
- **A regression from that, fixed** (`d058f70`). `666756c` printed
  `sqrt(1 - x^2)` as `1 - x^2^(1/2)` — again a different expression — because
  a missing `root()` was synthesised from already-rendered text. The demo
  found it; roots are now rewritten as display nodes and walked normally.
- **TeX and MathML renderers** (`666756c`, `693771a`): `toTeX()` and
  `toMathML()`. Found: both padded small exponents, `10^{-07}`.
- **Demo** (`2855343`, `07e62bc`): results through all four renderers,
  including the user-written `examples/text2d.hpp`.
- **`mod` and `round` agree with Maxima** (`de0061e`). Resolves both §1 numeric
  items and the §8 cross-check. Found: `mod(x, 0)` was NaN where Maxima gives
  `x`; and Maxima's own float `mod` loses precision at large magnitudes, so
  `mod(1e20, 3)` is deliberately exact here (1) rather than Maxima's 0.0.
- **`bool` and characters are no longer numbers** (`aac36c1`). Resolves the §1
  `Expr(true)` trap and its §8 test. Found: the wide character types got
  through too, into `Integer` as well as `Expr`, and so did `x + true`.
- **Feature tour** (`64c9f17`): `examples/tour.cpp`, a commented walk through
  every public feature that doubles as a quick-start guide. Writing it found
  two new §1 items: `Context::facts()` contradicting its documentation, and
  `limit` treating `ind` and `und` differently.
- **Boost.Process v2 replaces the hand-written transports** (`83f3bc8`).
  Resolves §6's first item, §3's `Sleep(1)` floor, and §2's handle leak and
  unchecked `WriteFile`; half of §2's ANSI item. On Windows a trivial round
  trip went from 15.5 ms to 0.04 ms and startup from ~131 ms to ~75 ms.
  Found: binding the child's end of a pipe to the io_context sent SBCL's
  overlapped-write completions to this process's completion port and corrupted
  memory at startup — Windows only, Maxima only, invisible to tests using
  cmd.exe. PLAN.md "Boost.Process" has the details.
- **Paths outside ASCII work** (`39dad91`). Resolves the rest of §2's
  ANSI item. Every internal path string is UTF-8 now, converted by the
  standard library, with no UTF-8 library added. Found: discovery read the
  environment through `std::getenv`, which is ANSI on Windows. And SBCL's own
  runtime reads its command line through the ANSI API, so it cannot open a
  core under a non-ASCII path whatever the caller passes. The library hands it
  8.3 short names instead, which only works on volumes that have them. PLAN.md
  "Paths outside ASCII" has the measurements.

Suite: 261 cases / 2805 assertions on Windows (GCC and clang-cl), 262 / 2802 on
Linux (222 when the review was written).

Still open and worth doing first: §1's NaN ordering, which is undefined
behaviour in the normaliser's sort.

---

## 1. Correctness — wrong answers

These produce a result that disagrees with Maxima, silently.

- [x] **`mod` in the numeric evaluator is `std::fmod`; Maxima's `mod` is
  floored.** `evalNumeric(mod(-7, 3))` gives **-1**; Maxima gives **2**.
  A closed form containing `mod` evaluates to a different number here than
  in Maxima. Fix: `a - b * std::floor(a / b)`. (`src/core/numeric.cpp`,
  `kBuiltins`; measured.)

  *Outcome:* fixed in `de0061e`, as `fmod` plus a sign correction rather than the
  suggested formula — that formula is how Maxima computes float `mod`, and it
  rounds: `mod(1e20, 3)` is exactly 1, but Maxima answers 0.0. The exact answer
  is kept and pinned. Also fixed, and not in the review: `mod(x, 0)` was NaN,
  where Maxima gives `x`.

- [x] **`round` is `std::round` (half away from zero); Maxima rounds half to
  even.** `round(2.5)` gives **3** here, **2** in Maxima. Fix:
  `std::nearbyint` under `FE_TONEAREST`, or `std::rint`. (Measured.)

  *Outcome:* fixed in `de0061e`, with half-to-even written out rather than
  `std::nearbyint`, which follows whatever rounding mode the host program has
  set. Confirmed against Maxima for negative halves too: `round(-2.5)` is -2 and
  `round(-0.5)` is an unsigned 0.

- [x] **`Expr(true)` is the Real `1.0`, and `Expr('a')` is the Real
  `97.0`.** The integral constructor template correctly excludes `bool` and
  `char`, but that only diverts them to the non-template `Expr(double)`,
  which accepts them by standard conversion. The exclusion achieves the
  opposite of its intent. Fix: make the `double` constructor a template
  constrained on `std::floating_point`, or `= delete` the `bool` and `char`
  overloads explicitly. Same hazard exists on `mx::Integer`'s constructors
  for anything that later adds a floating constructor. (Measured.)

  *Outcome:* fixed in `aac36c1`, with both suggested fixes together. The trap
  was wider than described: the wide character types were never excluded, so
  `Expr(u'a')` was the Integer 97 and `Integer(u'7')` compiled and meant 55,
  and `x + true` quietly built `1.0 + x`. Two concepts in `integer.hpp` —
  `IntegralNumber` and `BooleanOrCharacter` — now decide it for both classes,
  keeping `std::int8_t` and `std::uint8_t` (spelled with `signed char` and
  `unsigned char`) as numbers. The deleted constructors make the error name
  the type: `use of deleted function 'mx::Expr::Expr(T) [with T = bool]'`.

- [ ] **`Expr::parse("x!!")` gives `factorial(factorial(x))`.** In Maxima
  `!!` is the double factorial — a different function. The parser claims to
  be a subset of Maxima's grammar; on this input it is a superset with a
  different meaning. Either lex `!!` as its own token (→ `genfact`/
  `double_factorial`) or reject it. (Measured.)

- [ ] **`Expr::parse("a<b<c")` is accepted as `(a<b)<c`.** Maxima rejects
  chained relations. Same subset-vs-superset problem: make relations
  non-associative in the Pratt loop and throw. (Measured.)

- [ ] **NaN breaks `compareExpr`'s strict weak ordering.** `compareExpr`
  orders numbers through `double`; for NaN neither `<` nor `>` holds, so
  distinct NaNs compare *equal* to everything numeric, which violates the
  precondition of the `std::sort` calls in `normalize()` — that is
  undefined behaviour, not merely a wrong order. Also `Expr::real(NaN) ==
  Expr::real(NaN)` is false for two distinct nodes while their hashes are
  equal, and NaN prints as `nan`, which Maxima reads as a *symbol*. Decide:
  either reject NaN at `Expr::real` (throw) or give it a total order
  (`std::strong_order` on the bit pattern) and a spelling. (Measured.)

  *Partly done:* since `b71ccdf`, sending a NaN to Maxima throws `mx::Error`
  instead of arriving as the symbol `nan`. The ordering UB, the equality/hash
  mismatch and `str()` printing `nan` all remain.

- [ ] **Persistent-cache temp files can collide between processes.** The
  temporary is named with an in-process atomic counter, so two processes
  both produce `<hash>.reply.tmp0`, one truncates the other's partial write,
  and a corrupt file gets renamed into place. The length-prefixed format
  makes the reader *reject* it (so it self-heals as a miss), but the
  header comment claims "two writers race only to produce identical
  content", which is true of the target and false of the temp file. Add the
  PID or a random suffix. (`src/kernel/persistent_cache.cpp`)

- [ ] **`contains()` ignores `Opaque` text.** `solve` uses it to reject
  `[x = sin(x)]`-shaped non-solutions; a value that mentions the unknown
  only inside an Opaque node passes the check. Low likelihood, but the
  guard is the thing that makes "success really is a solution" true.

- [ ] **Discovery's error message hardcodes `sbcl.exe` on every
  platform.** Uses `kSbclName` everywhere else; two messages in
  `discoverMaxima` do not. (`src/kernel/discovery.cpp`)

- [ ] **`Context::facts()` does not do what its documentation says.**
  `context.hpp` promises "every assumption in force, this context's and its
  parents'", but it returns only the facts established in the scope itself.
  *Measured* by the feature tour (`64c9f17`): inside a nested scope, `facts()`
  listed only that scope's own declaration, although the outer scope had
  assumed `n > 0` and `x > 0` — and inheritance itself works, since
  `sqrt(x^2)` still simplified to `x` in there. *From reading `context.cpp`*:
  it evaluates a bare `facts()`, which is Maxima's currently active context,
  so it also ignores which `Context` it is called on — an outer scope's
  `facts()`, called while an inner one is active, would list the inner one's.
  Either implement the documented behaviour (`facts(name)` for this scope and
  each parent) or narrow the documentation to what it does.

- [ ] **`limit` reports `und` as a Failure but returns `ind` as a success.**
  `limit(abs(x)/x, x, 0)` comes back as the symbol `ind` — bounded, with no
  single value — where Maxima's `und` would have been a Failure. To most
  callers both mean "there is no limit", and nothing in `ops.hpp` says they
  differ, so a caller who only checks the `std::expected` takes `ind` as an
  answer. Decide whether `ind` is a Failure, or document it. (*Measured* by
  the feature tour, `64c9f17`, which now shows how to check for it.)

## 2. Robustness — a typo costs two minutes

- [x] **Resolved — outbound is now s-expressions; see PLAN.md "Outbound
  s-expressions".** *Original finding:* a *read* error is not an *eval*
  error, so `errcatch` never sees
  it. The request is `cppsend(id, errcatch(ratdisrep(<text>)))$` with the
  text spliced in raw. Any `$`, `;`, unbalanced paren or other syntax slip
  in the text makes Maxima's *reader* fail before `errcatch` is ever
  entered. No frame is produced, so `readFrame` waits the full
  `Config::timeout` (**two minutes** by default), throws `TimeoutError`,
  and then `recover()` kills and restarts the kernel — paying the
  transport's 2-second grace period on top. *Measured:* `eval("1$ 2")`,
  `eval("x; 3")` and `diff(Expr::opaque("x$ 0"), x)` each took the full
  timeout and threw `TimeoutError`; the session did come back healthy
  after each. The last one matters most: it reaches the stall through a
  *typed* public operation, not the raw escape hatch. Also measured:
  `eval("1) + cppsend(0, [7]")` succeeds — the spliced text is live
  Maxima code and can call the framing helper itself.

  This is reachable from: `Kernel::eval` / `evalPure` with user text,
  any `Expr::opaque(...)`, any `Symbol` or `Expr::function` head containing
  a character Maxima's reader treats specially, and `mx::parse` — no,
  `mx::parse` is safe: it quotes the text into a string literal for
  `parse_string`, which is exactly the right idea.

  Fixes, in increasing ambition:
  1. Send the text as a *string* and have the Lisp helper read it:
     `cppsend(id, "<escaped>")` with `errcatch(eval_string(...))` or
     `parse_string` inside the helper. Read errors then become ordinary
     caught errors with a message, and the frame always arrives. This is
     the real fix and it also closes every injection path at once.
  2. Failing that, validate symbol/function names at construction
     (`Symbol("x y")`, `Symbol("")`, `Expr::function("a(b", …)` are all
     accepted today and print as broken Maxima source — measured) and
     document that `eval` text must be a single expression.
  3. Add a test for the read-error path. There is none: `grep` finds no
     test of a malformed expression reaching the kernel.

  *Outcome:* fix 1 was done, and it made fix 2 unnecessary — `Symbol("x y")`
  now round-trips through Maxima as a bar-quoted symbol, so odd names are
  simply names. Fix 3 is `test_to_maxima.cpp`.

- [ ] **`recover()` after a timeout pays the 2-second grace period for
  nothing.** `kill()` waits up to 2 s for a child that has been *asked to
  quit* — but on a timeout it was not asked; it is busy computing and will
  never leave voluntarily. Pass a flag so recovery terminates immediately.
  (`child_process_win32.cpp`, `child_process_posix.cpp`)

- [ ] **`Kernel::eval` switches persistence off for the rest of the
  kernel's life.** `stateAccounted_ = false` is never reset. One diagnostic
  `eval("1+1")` and `Config::cacheDirectory` is dead until a new Kernel.
  The reasoning is sound (an unrecorded change cannot be keyed), but there
  is no `Kernel::evalPure`-style way back and nothing tells the caller.
  Either expose a `resetPersistence()` / document loudly, or make `eval`
  restart the kernel with the journal replayed — which *does* restore an
  accounted state.

- [ ] **`Context` destroyed out of LIFO order resets Maxima's active
  context to the wrong parent.** Move is deleted, but heap-allocated
  Contexts (or two on different stack frames) can still die in any order;
  the destructor does `context: <my parent>`, which for an *outer* scope
  destroyed first makes Maxima's active context the now-dead inner one's
  parent — i.e. wrong for the survivor. Either forbid it (track the stack
  and throw/terminate on misuse) or only reset `context:` when this one is
  the active one.

- [ ] **`PersistentCache::readField` trusts the stored length.** A corrupt
  or hostile `.reply` file with length `18446744073709551615` makes
  `text.resize()` throw `std::length_error` / `bad_alloc`, which escapes
  `evalPure` as a non-`mx::Error` exception. Cap the length (a reply cannot
  exceed the file size) and treat anything else as a miss.

- [ ] **A moved-from `Kernel` is a null pointer waiting to be
  dereferenced.** Every method does `session_->…` unchecked. Either
  document "moved-from is unusable" or throw `KernelError`.

- [ ] **`sharedKernel()` and static destruction order.** A function-local
  static Kernel is destroyed at exit; a user's own static that holds a
  `Context` (which holds a raw `Kernel*`) and outlives it will call into a
  dead object. Also: `ops.hpp` says `sharedKernel` is "Not thread-safe —
  see PLAN.md step 13", which is wrong on both counts (C++11 statics are
  thread-safe to initialise, and step 13 serialised the Kernel). Fix the
  comment; consider `Context` holding a `shared_ptr` or a weak reference.

- [x] **Win32 launch leaks every inheritable handle into every child.**
  `CreateProcessA(..., bInheritHandles = TRUE, ...)` with no
  `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` gives each Maxima child a copy of
  every inheritable handle in the host process, including other kernels'
  pipe ends. *Measured:* teardown with a sibling kernel alive is 5 ms, same
  as alone — masked here because `quit();` exits the process and `kill()`
  waits on the process handle rather than on pipe EOF. But it is the
  textbook cause of "pipe never reports EOF" bugs in host applications, and
  it is the reason the POSIX side needed a close-on-exec status pipe. Use
  the attribute list (or Boost.Process, see §6).

  *Outcome:* fixed by the move to Boost.Process (§6), whose Windows launcher
  passes `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` with only the child's three
  standard handles.

- [x] **Win32 uses the ANSI API family.** `CreateProcessA`,
  `GetEnvironmentStringsA`, `STARTUPINFOA`. A Maxima installed under a
  non-ASCII path — or a non-ASCII `Config::userDir` — will fail or be
  mangled. Use the `W` variants and convert.

  *Outcome:* half done in `83f3bc8`. Boost.Process launches with
  `CreateProcessW` and reads the environment with the wide API. But
  `session.cpp` still builds the SBCL argv with `path::string()`, which is
  narrow on Windows, so a non-ASCII install path is still mangled before it
  reaches the launch. Left open for that.

  *Outcome:* finished in `39dad91`. Every internal path string is
  UTF-8, which is what Boost.Process converts from, and discovery reads the
  environment through the wide API. An integration test starts Maxima through
  a junction named `mx_mæxima_中文` with a user directory named the same way.
  Getting there needed a workaround SBCL forced: its C runtime reads the core
  path from the ANSI command line, so the executable and core are passed by
  their 8.3 short names when they are not ASCII. On a volume with short names
  disabled a non-ASCII install still fails, now inside SBCL rather than here.

- [x] **Win32 `send` ignores `WriteFile`'s return value.** A failed or
  short write is silently dropped; the next `readFrame` then times out
  with a misleading diagnosis.

  *Outcome:* fixed in `83f3bc8`. `asio::write` writes everything or reports
  an error, and an error marks the transport closed, so the next `receive()`
  and `alive()` report the dead child at once.

- [ ] **The session mutex is held for the whole computation.**
  `cacheStats()` and `setTimeout()` take the same lock as `eval`, so both
  block for up to two minutes behind a running integral, and `setTimeout`
  cannot shorten an in-flight call. Separate a short lock for the
  bookkeeping from the long one for the pipe.

## 3. Performance

Fine at today's sizes; these are the walls you will hit.

- [ ] **`sizeof(Node)` is 160 bytes** (measured). Every node — every
  symbol, every `sin(x)` — carries two `cpp_int`s (64 bytes) plus a
  `double`, a `string` and a `vector`. `node.hpp` calls this a deliberate
  trade against `std::variant`; with `cpp_int` underneath the price
  doubled. A `std::variant<Integer, Rational, double, std::string,
  std::vector<Expr>>` payload, or a small tagged union, would halve the
  node and put the args vector for leaves out of existence.

- [ ] **`a + b` allocates about five times.** `Expr::add` takes a
  `std::vector<Expr>` by value (1), `normalize()` builds `flat` (2),
  `partitionNumbers` builds `numbers` and `rest` (3, 4), then
  `make_shared` (5), plus a `std::sort` over two elements. Given Boost is
  now a dependency and Boost.Container is *already being compiled* as part
  of it, `boost::container::small_vector<Expr, 4>` for operands and
  scratch is nearly free to adopt. A two-operand fast path in
  `operator+`/`operator*` (skip flatten/partition when neither side is the
  same kind and at most one is a number) would remove most of it.

- [x] **Resolved — the printer is a renderer now; see PLAN.md "Renderers".**
  *Original finding:* the printer runs the normaliser. `negativeTerm()` rebuilds a
  product with `Expr::mul(std::move(factors))` — allocation, flatten, sort
  — for every negative term of every sum it prints, and `render(Add)`
  copies the terms vector to rotate it. Printing should be a read-only
  walk; carry the sign as a flag instead. Also `render()`'s `context`
  parameter is entirely unused (`static_cast<void>(context)`) — dead
  parameter, remove it.

  *Outcome:* both gone. The sign is a flag on the display tree, the dead
  parameter went with the rewrite, and a bug nobody had noticed went too:
  `x - (1 + y)` printed as `x - 1 + y`, which re-parses to a different
  expression. `tests/test_render.cpp` pins the round trip now.

- [ ] **`readFrame` is O(n²) on large replies.** Every 4 KB chunk appends
  to `buffer` and then `buffer.find(end)` searches *from the beginning*.
  Not yet visible in practice — `expand((x+y+z)^40)` is a 6 KB reply and
  takes 16 ms end to end — but it is quadratic in reply size by
  construction. Search from `max(0, oldSize - end.size())` instead, and
  raise the transport chunk size (64 KB) — the 4 KB buffer means a 1 MB
  reply is 256 syscalls and 256 searches.

- [x] **Win32 `receive` polls with `Sleep(1)`.** `PeekNamedPipe` +
  `Sleep(1)` in a loop; on a default Windows timer that sleep is 1–15 ms,
  so every round trip carries that latency floor. *Measured:* **15.5 ms
  per trivial `evalPure`** (200 cache-missing `1+i` calls), which is
  almost exactly Windows' default 15.625 ms scheduler tick — the cost is
  the sleep, not Maxima. That is 3 s for 200 questions, and it is the
  number behind "a round trip costs milliseconds" in the docs. The comment
  says "Step 13 replaces this with a dedicated reader thread" — it did
  not. Options: overlapped I/O with an event, a reader thread feeding a
  condition variable, or Boost.Process's async pipes (§6).

  *Outcome:* fixed in `83f3bc8` with Asio's pipes, which wait on the
  completion port. Same 200-call measurement: **0.04 ms** per trivial
  `evalPure`, and 1.5 ms per `expand` round trip (was 15.6 ms).

- [ ] **`Context` construction clears the reply cache just to read a
  name.** `evaluateOrThrow(kernel, "context")` goes through `evalTracked`,
  which clears the in-memory cache, before anything has changed. Use
  `evalPure` for the read.

- [ ] **`evalNumeric`'s `walk()` allocates a `std::vector<double>` per
  function call**, and `isEvaluable` builds failure strings it then
  discards. Use a small stack array (max builtin arity is 2 except
  `max`/`min`) and pass `nullptr` for the failure sink.

- [ ] **`Compiled::pushConstant` dedups with `std::find` on `double ==`.**
  Merges `-0.0` with `0.0` (sign of zero lost — harmless in practice) and
  never merges NaN. Compare bit patterns if you want exact dedup; it is
  O(n) per constant either way, fine for expression sizes seen here.

## 4. API ergonomics

- [ ] **No `operator<<` and no `std::formatter`** for `Expr`, `Integer`,
  `Symbol`. Every print in the demo is `.str()`. Ten lines, large quality
  of life gain. *(Now also the natural place to hang a format spec that
  selects a renderer: `std::format("{:tex}", e)`.)*

- [ ] **`Expr` has no ordering.** It cannot be a `std::map` key, cannot be
  sorted, cannot be put in a `std::set` — yet a total order already exists
  in `detail::compareExpr`. Expose it as `operator<=>` (fixing the NaN
  case first, §1).

- [ ] **No local structural substitution.** `subst(f, x, 5)` is a Maxima
  round trip for what is a tree rewrite. A `replace(expr, symbol, value)`
  in `src/core` — no kernel — would be the single most-used helper in any
  numeric-driver code, and it composes with `Compiled`.

- [ ] **No traversal helpers.** `args()` is enough to write a recursion,
  but a `visit`/`transform`/`anyOf` would stop every caller writing the
  same one (`contains` and `mentionsSymbol` in this codebase are already
  the same function twice).

- [ ] **The wire format leaks through `Kernel::eval`.** *(Partly done:
  `eval`/`evalPure`/`evalTracked` now have `const Expr &` overloads, so a
  caller can send structure. The reply is still raw text.)* It returns
  `Reply::value` as raw s-expression text. A public `Kernel::evalExpr`
  returning `std::expected<Expr, Failure>` — which is what `ops.cpp`'s
  private `evaluate()` already is — would let users who need a Maxima
  function this library has not wrapped get an `Expr` back without
  parsing s-expressions themselves. Keep `eval` for the raw case.

- [ ] **The operation set is thin for "basic workable".** Missing and
  cheap to add given the existing `evaluate()` helper: `is(...)`
  (ask Maxima a predicate under the current assumptions — the natural
  partner to `Context`), `taylor`, `trigsimp`/`trigexpand`/`radcan`,
  `sum`/`product`, `partfrac`, `float`/`numer` (Maxima-side numeric
  evaluation, complementing the local one), `lhs`/`rhs`, `coeff`,
  `nroots`/`realroots`/`find_root`, `ode2`. Matrices can wait; they need a
  typed node to be pleasant.

- [ ] **`functions.hpp` puts `sin`, `cos`, `log`, `abs`, `exp`, `sqrt` in
  `namespace mx`.** Under `using namespace mx;` with `<cmath>` in scope,
  `abs(x)` for an `int x` now has a viable `mx::abs(Expr)` candidate via
  the implicit constructor; overload resolution still picks the
  `int`/`double` one, but it is the kind of thing that turns into an
  ambiguity the day someone adds an overload. Consider a sub-namespace
  (`mx::fn`) or accept it and document "don't `using namespace mx`".
  Also: `minusInf()` vs `inf()` naming; `tanh`, `asinh`, `acosh`, `atanh`,
  `erf`, `floor`, `ceiling`, `signum` are in the numeric builtin table but
  have no builder; `%gamma` is a Maxima constant the numeric layer does
  not know (`std::numbers::egamma` exists); `%phi` is known but
  undocumented.

- [ ] **`simplify` is `ratsimp`.** Documented, but the name promises more
  than it does; `ratsimp` as the public name (with a doc pointing at
  `trigsimp`, `radcan`) is more honest and matches Maxima's vocabulary,
  which the rest of `ops.hpp` already does.

- [x] **`isUnevaluated(result, "list")` is used to mean "is a list".**
  A misnomer that reads as "Maxima failed" at every call site in `solve`.
  Add `isList()`.

  *Outcome:* done in `b71ccdf`; `solve` now calls `isList()`.

- [ ] **`wrongKind()` prints the kind as an integer.** "expression is not
  an integer (kind 4)". Add a `to_string(Kind)` / `kindName()` — it is
  also wanted for tests and logging.

- [ ] **`Bindings` is keyed by `std::string`; `Compiled` takes
  `span<const Symbol>`.** Two spellings of "which symbol". Accepting
  `Symbol` in `Bindings` (or a transparent comparator over both) would
  make the numeric API read consistently.

- [x] **Printing `-1*x` and `(-1*x)^2`.** `-(x+1)` prints `-1*(1 + x)`
  and `(-x)^2` prints `(-1*x)^2` (measured). Valid Maxima, but every
  user will read it as a bug. The printer already special-cases a leading
  `-1` inside sums; extend it to products at the top level and inside
  `Pow`.

  *Outcome:* resolved by the renderer layer in `666756c`, which carries a sign
  as a flag rather than a `-1` factor. `-(1 + x)` and `-x - y` are pinned in
  `test_render.cpp`, and `(-x)^2` is in its round-trip corpus.

- [ ] **`x**2` and `1e400` are parse errors.** Maxima accepts `**` as
  `^`; accept it. `1e400` overflows `double` — either throw a clearer
  message ("out of range") or produce `inf`.

## 5. Documentation drift

The comments are unusually good at saying *why*, which makes the stale ones
stand out. All refer to plan steps as future work that has since shipped:

- [ ] `include/mx/expr.hpp`, `Expr::parse` doc: a paragraph is truncated
  mid-sentence ("…becomes an Opaque node holding its") and then
  contradicted by the next one. Delete the stale paragraph.
- [x] `include/mx/reply.hpp`: "Text only for now: PLAN.md step 7 adds the
  reader… Until then this is the rawest useful thing". *(Fixed.)*
- [x] `include/mx/kernel.hpp`: "This is the whole public surface for now…
  structured expressions arrive with the term layer (PLAN.md steps 7-9)".
  *(Fixed in `b71ccdf`.)*
- [ ] `include/mx/ops.hpp`, `sharedKernel`: "Not thread-safe — see PLAN.md
  step 13". Wrong, see §2.
- [ ] `include/mx/context.hpp`: "so that PLAN.md step 14's cache key can
  include them".
- [x] `src/core/printer.cpp` header: "which is also what the Expr →
  Maxima direction of the translation layer will need (PLAN.md step 9)".
  *(Fixed — and now the opposite is true: the printer is not on the path to
  Maxima at all.)*
- [ ] `src/transport/child_process_win32.cpp`: "Step 13 replaces this
  with a dedicated reader thread feeding a bounded queue".
- [ ] `include/mx/config.hpp`, `timeout`: "Maxima keeps computing until it
  is killed" — true, but `recover()` *does* kill it on timeout; say so,
  since the current wording suggests a runaway process is left behind.
- [ ] `src/wire/from_maxima.cpp`: `mapInteger` and `mapRational` have
  `Opaque` fallbacks for "digits that do not parse" that can no longer
  happen — the lexer guarantees digits and `Integer` is unbounded. Dead
  code; remove or `assert`.
- [ ] `src/kernel/persistent_cache.hpp`: the concurrency claim (see §1).

## 6. Things a library could do instead

You said dependencies are fine. With Boost already fetched, these are the
candidates, most valuable first.

- [x] **Boost.Process (v2) for `child_process_win32.cpp` /
  `child_process_posix.cpp` / `process_env.cpp` / `win32_process_utils.hpp`
  (~560 lines).** It handles the handle-inheritance list, Unicode paths,
  argument quoting, environment merging, and — with Boost.Asio, which it
  sits on — *asynchronous reads with a deadline*, which removes both the
  Win32 `Sleep(1)` poll and the "cannot abandon a blocking ReadFile"
  problem in one move. It is header-only. The cost is that Asio is a big
  header (compile time) and a new concept in the codebase. The transport
  interface is already abstract, so this is a drop-in behind `ITransport`
  and FakeTransport keeps the tests honest. Worth doing; the hand-written
  code is careful but §2 lists four platform-specific holes in it.

  *Outcome:* done in `83f3bc8`. One correction to the above: Boost.Process v2
  is not header-only — it is a compiled library, fetched and built by CPM like
  the rest of Boost, and linked privately. Asio's compile-time cost is
  confined to `child_process.cpp`. `process_env.cpp` stays, smaller, because
  the session merges overrides into the inherited environment.

- [ ] **`boost::container::small_vector` for `Node::args` and the
  normaliser's scratch vectors.** Already compiled as a transitive
  dependency of multiprecision. See §3.

- [ ] **`std::variant` for the Node payload.** Standard library, no
  dependency. See §3.

- [ ] **A real database for the persistent cache?** SQLite would give
  bounded size (there is *no eviction* today — `Config::cacheDirectory`
  grows forever), atomic multi-entry writes and a proper cross-process
  story. It is a compiled dependency, though, and the one-file-per-entry
  design is genuinely simple. Recommendation: keep the files, add a size
  cap with LRU-by-mtime eviction, and fix the temp-name collision (§1).

- [ ] **Keep hand-written:** the LRU cache (70 lines, nothing to gain),
  FNV-1a (stability across builds is the whole point; `std::hash` and
  `boost::hash` do not promise it), the s-expression reader (small,
  tested against golden files), and the Pratt parser (you asked for no
  third-party parser, and it is the right call — its bugs in §1 are
  semantic, not structural).

- [ ] **A fuzz target for the two parsers.** libFuzzer/AFL on
  `parseSExpr` and `Expr::parse` is an afternoon and is how hand-written
  readers earn trust. `kMaxSExprDepth` exists, so someone already thought
  about hostile input; a fuzzer would find what the depth limit does not.

## 7. Build, repo, process

- [ ] **No `LICENSE` file.** README has a "Licence" section about
  Maxima's GPL and the process boundary, but this library's own licence
  is never stated anywhere. That is the first thing a consumer looks for.

- [ ] **`.idea/` is tracked** (7 files). The `.gitignore` only excludes a
  subset. Either commit the whole project config deliberately or ignore
  the directory.

- [ ] **Warnings are clean but nothing enforces it.** Add
  `-Wall -Wextra -Wpedantic -Wshadow -Wconversion` (`/W4` for MSVC) to the
  library target, and `-Werror` in CI. The sweep for this review found
  zero, so it costs nothing to turn on now and it will catch the first
  regression.

- [ ] **No CI.** Three toolchains were verified by hand for the Boost
  change (GCC/Windows, clang-cl/Windows, GCC/Linux). A GitHub Actions
  matrix running `ctest -LE maxima` (no Maxima needed) on all three, plus
  one job with Maxima installed for the integration suite, would make
  that automatic. The LP64 `long long` ambiguity that only Linux caught is
  the argument.

- [ ] **No `.clang-format` / `.clang-tidy`.** The code is consistently
  styled, which means a format file already exists in someone's head;
  commit it.

- [ ] **`std::getenv` triggers MSVC's deprecation warning** under
  clang-cl (seen in the build log). `_CRT_SECURE_NO_WARNINGS` on the
  target, or `_dupenv_s` under `_WIN32`.

- [ ] **`BUILD_SHARED_LIBS OFF` is passed to Boost through CPM
  `OPTIONS`.** CPM sets those as cache variables, so it also pins *this*
  project's default. Harmless today (the library is static regardless),
  but it will surprise whoever adds a shared-library option.

## 8. Tests — what is missing

The suite is strong where it counts (golden transcripts from real Maxima,
FakeTransport for the protocol, randomised bignum cross-checks against
Maxima itself). Gaps, all cheap:

- [x] The read-error path (§2): a malformed `eval` string, an `Opaque`
  with a `$`, a `Symbol` with a space. *(`test_to_maxima.cpp`, against a
  live kernel, with a 10 s bound that a regression to the old behaviour
  would blow through.)*
- [x] `Expr(true)`, `Expr('a')` — pin the intended behaviour. *(`aac36c1`:
  compile-time checks in `test_expr.cpp` and `test_integer.cpp` for both
  directions, which fail against the previous code.)*
- [ ] Out-of-order `Context` destruction.
- [ ] Two threads sharing one `Kernel` (the README promises it is safe;
  nothing exercises it).
- [ ] `Kernel` after move.
- [x] `mod`/`round` against Maxima's own answers — the numeric builtins
  are the one place the library computes something Maxima also computes,
  so cross-check them the way `test_integer.cpp` cross-checks bignums.
  *(`de0061e`: `test_numeric.cpp` checks 66 `mod` pairs and 12 `round` values
  against a live kernel. The other builtins are not yet cross-checked.)*
- [ ] Parser: every Maxima operator the subset *claims* to reject should
  have a test that it does reject it, and `!!`, `**`, chained relations
  should be decided and pinned.

---

## Not problems

Things I checked that are fine and that a reviewer might flag anyway.

- `sharedKernel()` as a function-local static is thread-safe to
  construct; the comment is wrong, not the code.
- `operator/` producing `x*0^(-1)` for division by zero: deliberate,
  Maxima is the one that objects, and it does.
- `x - x` staying `x - x`: correct, one canonicaliser.
- `-0.0 == 0.0` as `Expr`: true, with equal hashes; consistent.
- `Integer`'s small-value fast paths: verified allocation-free.
- `isupper`/`islower` in `decodeMaximaName` are locale-sensitive in
  principle; Maxima symbol names reaching here are ASCII in practice.
- The 4 KB transport chunk and `find`-from-zero are the same issue as
  §3's `readFrame`; listed once.
