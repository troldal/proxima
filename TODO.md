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

Updated after `7fbed5a`. Resolved findings are ticked where they stand, with
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
- **The rest of §1, one commit each** (`c70adc7` … `e81c66c`). Every
  wrong-answer finding is now closed. Each fix was checked against what real
  Maxima does before changing anything. Found on the way: `0.0 == -0.0`, but
  MSVC's `std::hash<double>` hashes the bit pattern, so the two could hash
  differently under clang-cl; `(a < b) < c` is legal Maxima, so only the
  unparenthesised chain is refused; a test meant to prove NaN is refused
  passed vacuously, because `Expr(nan);` declares a variable; and Maxima's
  `global` context holds 75 built-in type facts that `facts()` must not report
  as assumptions.
- **§2's open items, one commit each** (`3133650` … `539d93c`). Where the
  failure could be reproduced, a test showed it before the fix; where the old
  behaviour was a crash (a moved-from Kernel, a Context outliving its Kernel)
  it could not be. Found on the way: automatic recovery replays the journal
  into a fresh process yet left persistence switched off; a Context ended out
  of order took the survivor's *own* facts with it, not just inherited ones,
  and Maxima quietly recreates a context that `context:` names after it was
  killed; and splitting the session lock exposed a gap that was already
  there, listed as a new §2 item and closed in `6bc0f7d`.
- **MSVC** (`56389be`). The non-ASCII path tests failed under MSVC because
  their literals held raw characters, which MSVC reads in the ANSI code page
  unless given `/utf-8`. The tests now spell the name in bytes and code units,
  and every target compiles with `/utf-8` under MSVC. Verified with `cl`.
- **§3, measured before and after each change** (`bfcdea6` … `8bb6ca4`), in
  a GCC Release build with the old and new library run alternately. A first,
  single-run comparison of the node layout suggested a 45% slowdown; repeated
  and interleaved it was noise, which is why every figure below is a median.
  Found on the way, and more serious than anything in §3: replies were
  printed under Maxima's Lisp print limits, so any result with more than 100
  terms or nested deeper than 15 came back silently truncated, and the reader
  took a bar-quoted symbol such as `|123|` for a number (`d670d17`, now in §1).
- **§4, one commit each** (`2121eb4` … `e310a01`). Every new operation's reply
  was probed in Maxima before it was wrapped. Found on the way: the canonical
  order compared numbers as doubles, so 2^100 and 2^100 + 1 were equivalent
  though unequal; `transform` could not use `==` to see an operand come back
  unchanged, since 0.0 == -0.0; the `<cmath>` naming hazard reached `abs`, `gcd`
  and `pow`, not only the builders; and `ode2` writes onto the pipe when it
  fails. Verified with GCC and clang-cl on Windows, with MSVC, and with GCC on
  Linux.
- **MinGW/clang 22** (`23e0449`). `std::set<Expr>` did not compile: libc++ 22's
  tree replaces a `std::less<T>` comparator with the transparent `std::less<>`,
  whatever `std::less<T>` has been specialised to, and that calls `<`, which
  `Expr` deliberately lacks. A ten-line program with no library code shows the
  same. The specialisations are gone; ordered containers name
  `mx::CanonicalLess`.
- **§5, one commit each** (`3ea48d5` … `edd45a1`). Three of the seven had
  already been fixed by earlier work and are ticked with those commits. Found
  on the way: the dead `Opaque` fallback in `mapRational` was not dead — a
  `(RAT 1 0)` became the text `(1/0)`, which passes for an answer — and two
  more comments, in `errors.hpp` and `tests/CMakeLists.txt`, still described
  shipped work as future.
- **§6, the recommended items only** (`e4dd610` … `9baaffe`); the rest are
  struck with the reason. The persistent cache now has a size limit. The fuzz
  targets found six bugs within minutes of their first runs, none of which the
  suite had: a stack overflow on deeply nested text, long zero-padded integers
  read in octal, a `;` that made the reply reader loop until memory ran out,
  and three ways printed expressions read back in a different shape. Found on
  the way: a fixed depth limit that was safe for GCC still overflowed the
  stack in clang-cl's and MSVC's Debug builds, so the parser now measures the
  stack it uses; and running three compilers' suites at once showed that the
  non-ASCII path test shared one directory between runs.
- **§8** (`dc0f03b`, `7fbed5a`). Two of the four open items already had tests,
  written with their §2 fixes. The other two are new tests, and both passed on
  their first run: the behaviour was right, only unpinned.

Suite: 322 cases / 4734 assertions on Windows (GCC and clang-cl), 323 / 4732 on
Linux (222 when the review was written).

§1 to §6 and §8 are closed. What remains is §7 (build and process).

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

- [x] **`Expr::parse("x!!")` gives `factorial(factorial(x))`.** In Maxima
  `!!` is the double factorial — a different function. The parser claims to
  be a subset of Maxima's grammar; on this input it is a superset with a
  different meaning. Either lex `!!` as its own token (→ `genfact`/
  `double_factorial`) or reject it. (Measured.)

  *Outcome:* fixed in `c70adc7` by lexing `!!` as its own token, greedily, as
  Maxima's lexer does: `x!!!` is `(x!!)!`, and only `x! !` with a space is a
  factorial taken twice. It maps to `double_factorial`, which Maxima reads to
  the same `%DOUBLE_FACTORIAL` noun, so no wire mapping had to change. Checked
  against Maxima for `x!!`, `x!!!`, `x! !`, `2*x!!` and `5!!` = 15.

- [x] **`Expr::parse("a<b<c")` is accepted as `(a<b)<c`.** Maxima rejects
  chained relations. Same subset-vs-superset problem: make relations
  non-associative in the Pratt loop and throw. (Measured.)

  *Outcome:* fixed in `52ad2d0`. Maxima refuses any unparenthesised chain,
  mixed operators included (`a < b = c`), but accepts `(a < b) < c`,
  `a = (b = c)` and a relation as an argument; the parser now matches all of
  that, reporting the offset of the second relation where Maxima puts its
  caret. The printer already kept the parentheses a nested relation needs, and
  a test now pins that, since printing without them would produce exactly what
  is refused.

- [x] **NaN breaks `compareExpr`'s strict weak ordering.** `compareExpr`
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

  *Outcome:* fixed in `d5a227d` by rejecting NaN at `Expr::real`, which ends
  all three symptoms at once and also covers arithmetic that folds to NaN
  (`inf - inf`, `0.0 * inf`). The total order was not chosen: ordering by bit
  pattern would also have made `0.0` and `-0.0` unequal. That turned up a
  sibling bug — the two are equal, but MSVC's `std::hash<double>` hashes their
  bits differently — so the hash now treats them alike. Infinities are
  unaffected, and the NaN branches in the MathML renderer and the wire
  encoder, now unreachable, are gone.

- [x] **Persistent-cache temp files can collide between processes.** The
  temporary is named with an in-process atomic counter, so two processes
  both produce `<hash>.reply.tmp0`, one truncates the other's partial write,
  and a corrupt file gets renamed into place. The length-prefixed format
  makes the reader *reject* it (so it self-heals as a miss), but the
  header comment claims "two writers race only to produce identical
  content", which is true of the target and false of the temp file. Add the
  PID or a random suffix. (`src/kernel/persistent_cache.cpp`)

  *Outcome:* fixed in `f2de237` with both: the temporary is
  `<entry>.tmp-<token>-<n>`, the token random per process and mixed with the
  process id and the clock, so it holds without an entropy source and across
  machines sharing a directory. The comments now say what is and is not raced.
  A test plants another writer's half-written temporaries beside an entry and
  checks `insert` leaves them alone; the cross-process guarantee itself rests
  on the token, which a second test checks.

- [x] **`contains()` ignores `Opaque` text.** `solve` uses it to reject
  `[x = sin(x)]`-shaped non-solutions; a value that mentions the unknown
  only inside an Opaque node passes the check. Low likelihood, but the
  guard is the thing that makes "success really is a solution" true.

  *Outcome:* fixed in `44669d5`. Opaque text is not parsed: a plain name
  counts where it stands as a whole identifier outside string literals, and a
  name that is not a plain identifier, such as `x y`, counts wherever it
  appears once Maxima's backslash escapes are removed. It errs towards true,
  which is the direction the guard needs.

- [x] **Discovery's error message hardcodes `sbcl.exe` on every
  platform.** Uses `kSbclName` everywhere else; two messages in
  `discoverMaxima` do not. (`src/kernel/discovery.cpp`)

  *Outcome:* fixed in `722d9c9`; the test checks the message names the
  platform's own executable.

- [x] **`Context::facts()` does not do what its documentation says.**
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

  *Outcome:* implemented as documented, in `760f9a7`. Maxima's `facts(name)`
  lists one context's own facts and nothing of its ancestry, so each Context
  records its parent in a small locked registry, and `facts()` asks about this
  scope, every enclosing one, and then the first context the library did not
  open — normally `initial`, where facts assumed outside any Context live —
  innermost first. It stops short of `global`, whose 75 facts are Maxima's own
  type system (`kind(%e, irrational)`), not assumptions. Called on an outer
  scope while an inner one is open, it now describes the outer scope.

- [x] **`limit` reports `und` as a Failure but returns `ind` as a success.**
  `limit(abs(x)/x, x, 0)` comes back as the symbol `ind` — bounded, with no
  single value — where Maxima's `und` would have been a Failure. To most
  callers both mean "there is no limit", and nothing in `ops.hpp` says they
  differ, so a caller who only checks the `std::expected` takes `ind` as an
  answer. Decide whether `ind` is a Failure, or document it. (*Measured* by
  the feature tour, `64c9f17`, which now shows how to check for it.)

  *Outcome:* `ind` is a Failure, in `e81c66c`, with a message saying the
  expression stays bounded without settling. Infinite limits — `inf`, `minf`,
  and `infinity`, Maxima's complex infinity for 1/x at 0 from both sides —
  remain values, and `ops.hpp` now says which answers are which. The tour no
  longer shows a hand-written check for `ind`.

- [x] **Large results came back silently truncated.** *Found while measuring
  §3's `readFrame`, whose large replies never grew.* Maxima runs with Lisp's
  `*print-length*` at 100 and `*print-level*` at 15, and the helper printed
  every reply under them: a sum of more than 100 terms arrived as its first 100
  and `...`, anything nested deeper than 15 levels as `#`, and the reader took
  both for ordinary symbols. `expand((x+y+z)^40)`, 861 terms, mapped to an Add
  of 100 — a wrong answer that looked right. The "6 KB reply" §3 quotes for
  that expression was the truncated one; whole, it is 75 KB.

  *Outcome:* fixed in `d670d17`. The helper binds `*print-length*`,
  `*print-level*` and `*print-lines*` to `nil`, and `*print-base*` and
  `*print-radix*` to their defaults, and the reader refuses a bare `...` or
  `#`, so a truncation can no longer pass silently. Making that refusal safe
  found a second bug: bar-quoted symbols went through the same path as bare
  atoms, so `|123|` read as the integer 123. Quoted symbols now have their own
  token kind. Tests written first failed on the old code.

- [x] **A Real infinity prints as the symbol `inf`.** *Found by fuzzing, after
  the review.* `Expr::real` allows infinities, and they reach Maxima as `inf`
  and `minf`, so that is how one prints — and `Expr::parse` reads `inf` back as
  the symbol, a different expression. A Real infinity is also reached from
  finite input: a fold of an exact number with a real converts the exact one
  to double, and overflows silently, so an integer of a few hundred digits
  divided by `5.0` is `inf`. The literal `1e400`, meanwhile, is refused as out
  of range (`e310a01`). Options: refuse a fold that overflows to infinity, as a
  fold to NaN is refused; turn a Real infinity into the symbol `inf` or `minf`
  at construction, which changes how infinities fold; or keep both, and
  document that an infinity does not survive printing. `fuzz_parser` skips its
  round-trip check for expressions holding one until this is decided.

  *Outcome:* both remedies, in `98a69db`, since Maxima does both. It has no
  floating-point infinity — `inf + 1.0` stays a sum there — and it refuses
  `10^400*5.0` and `1.0e308*10.0` with `FLOATING-POINT-OVERFLOW` (checked).
  `Expr::real(±inf)` is now the symbol `inf` or `minf`, so it prints and reads
  back as itself, and does not fold: `Expr(inf) + Expr(-inf)` stays a sum rather
  than throwing for NaN. A numeric fold whose result is not finite throws
  `mx::Error`; with no infinite Real left to be an operand, that is always an
  overflow. Dividing by a real whose reciprocal would overflow stays a negative
  power, as other unfolded divisions do. The encoders' infinite-Real branches
  are gone, and `fuzz_parser` checks the round trip for everything again.

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

- [x] **`recover()` after a timeout pays the 2-second grace period for
  nothing.** `kill()` waits up to 2 s for a child that has been *asked to
  quit* — but on a timeout it was not asked; it is busy computing and will
  never leave voluntarily. Pass a flag so recovery terminates immediately.
  (`child_process_win32.cpp`, `child_process_posix.cpp`)

  *Outcome:* fixed in `3133650`. `ITransport` gained `terminate()`, which ends
  the child without the grace period, and recovery uses it; the destructor
  still asks Maxima to quit. *Measured:* the real-Maxima timeout test went from
  2.56 s to 0.56 s, restart included.

- [x] **`Kernel::eval` switches persistence off for the rest of the
  kernel's life.** `stateAccounted_ = false` is never reset. One diagnostic
  `eval("1+1")` and `Config::cacheDirectory` is dead until a new Kernel.
  The reasoning is sound (an unrecorded change cannot be keyed), but there
  is no `Kernel::evalPure`-style way back and nothing tells the caller.
  Either expose a `resetPersistence()` / document loudly, or make `eval`
  restart the kernel with the journal replayed — which *does* restore an
  accounted state.

  *Outcome:* fixed in `cf6a0eb` with both halves, but not by making `eval`
  restart — that would discard exactly the definitions `eval` is used to
  make. `Kernel::persistenceActive()` reports the state and `Kernel::restart()`
  replays the journal into a fresh Maxima, which resumes persistence. Found:
  automatic recovery after a death or timeout did the same replay yet left
  persistence off; it now resumes there too.

- [x] **`Context` destroyed out of LIFO order resets Maxima's active
  context to the wrong parent.** Move is deleted, but heap-allocated
  Contexts (or two on different stack frames) can still die in any order;
  the destructor does `context: <my parent>`, which for an *outer* scope
  destroyed first makes Maxima's active context the now-dead inner one's
  parent — i.e. wrong for the survivor. Either forbid it (track the stack
  and throw/terminate on misuse) or only reset `context:` when this one is
  the active one.

  *Outcome:* fixed in `255e5d1`, by neither suggestion. Tests written first
  showed it worse than described: the surviving inner scope lost its *own*
  facts too, not only inherited ones, Maxima was left in a dead context, and a
  restart could not rebuild the survivor. Probing Maxima showed why only
  resetting `context:` would not do: `killcontext` on an outer context leaves
  its subcontexts orphaned, and `context:` naming a killed context quietly
  recreates it empty. An outer scope that ends with inner scopes open now
  waits, keeping its facts and replay entries, and is torn down with the last
  of them.

- [x] **`PersistentCache::readField` trusts the stored length.** A corrupt
  or hostile `.reply` file with length `18446744073709551615` makes
  `text.resize()` throw `std::length_error` / `bad_alloc`, which escapes
  `evalPure` as a non-`mx::Error` exception. Cap the length (a reply cannot
  exceed the file size) and treat anything else as a miss.

  *Outcome:* fixed in `a2a55fd`, capping each field at what remains of the
  file. The test reproduced the `std::length_error` first.

- [x] **A moved-from `Kernel` is a null pointer waiting to be
  dereferenced.** Every method does `session_->…` unchecked. Either
  document "moved-from is unusable" or throw `KernelError`.

  *Outcome:* both, in `c1b91eb`: every method goes through one checked
  accessor that throws `KernelError`, and the move operations say so.

- [x] **`sharedKernel()` and static destruction order.** A function-local
  static Kernel is destroyed at exit; a user's own static that holds a
  `Context` (which holds a raw `Kernel*`) and outlives it will call into a
  dead object. Also: `ops.hpp` says `sharedKernel` is "Not thread-safe —
  see PLAN.md step 13", which is wrong on both counts (C++11 statics are
  thread-safe to initialise, and step 13 serialised the Kernel). Fix the
  comment; consider `Context` holding a `shared_ptr` or a weak reference.

  *Outcome:* fixed in `2a74041` with the weak reference. A Kernel owns a
  lifetime token that moves with its session and expires before the session
  is destroyed; a Context holds a `weak_ptr` to it, so once the Kernel is gone
  its operations throw `KernelError` and its destructor does nothing. That
  covers any Context outliving its Kernel, not only a static one outliving
  `sharedKernel()`. The comment is corrected.

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

- [x] **The session mutex is held for the whole computation.**
  `cacheStats()` and `setTimeout()` take the same lock as `eval`, so both
  block for up to two minutes behind a running integral, and `setTimeout`
  cannot shorten an in-flight call. Separate a short lock for the
  bookkeeping from the long one for the pipe.

  *Outcome:* fixed in `539d93c` as suggested: a pipe lock for the conversation
  and a state lock for everything else, with `readFrame` re-reading the
  timeout each poll so `setTimeout` shortens a waiting call. Splitting them
  meant the journal can change mid-computation, so `evalPure` now caches an
  answer only if a state generation counter is unchanged across the call.
  `evalPure`'s cache lookups deliberately still take the pipe lock — see the
  next item.

- [x] **A Context's statement and its journal record are two separate
  calls.** `Context::assume` sends `assume(...)` through `evalTracked`, then
  records it with `remember()`; `~Context` does `forget()`, then
  `killcontext`. Each is locked on its own, so another thread's `evalPure` can
  run in between, compute under Maxima's new state, and file the answer under
  the journal's old one — in the in-memory cache, and in `cacheDirectory`,
  where it outlives the process. The window predates `539d93c` (the old single
  lock was also released between the two calls); the generation counter added
  there closes only the case where the journal changes *during* a
  computation. Fix: one session call that evaluates and records (or forgets
  and evaluates) under the pipe lock, so the change and its record are
  atomic. Found by reasoning while splitting the lock, not yet measured.

  *Outcome:* fixed in `6bc0f7d` as proposed. `MaximaSession::converseAtomically`
  runs its steps under one hold of the pipe lock, offering `evalTracked`,
  `remember` and `forget`, and Context makes every change that way: opening a
  scope (reading the active context, `supcontext`, the record), each
  assumption or declaration with its record, and each teardown. No public API
  was added — Context reaches the session through its existing friendship with
  Kernel. The race is a timing window with no deterministic reproduction, so
  there was no failing test first; the new test pins the guarantee instead: a
  question asked mid-conversation waits for the record, and its answer is then
  cached under the recorded state.

## 3. Performance

Fine at today's sizes; these are the walls you will hit.

- [x] **`sizeof(Node)` is 160 bytes** (measured). Every node — every
  symbol, every `sin(x)` — carries two `cpp_int`s (64 bytes) plus a
  `double`, a `string` and a `vector`. `node.hpp` calls this a deliberate
  trade against `std::variant`; with `cpp_int` underneath the price
  doubled. A `std::variant<Integer, Rational, double, std::string,
  std::vector<Expr>>` payload, or a small tagged union, would halve the
  node and put the args vector for leaves out of existence.

  *Outcome:* done in `bfcdea6` with the `std::variant` payload. A node is 96
  bytes, 112 allocated (was 176), and a leaf carries no operand vector. Nothing
  outside `expr.cpp` read Node's fields. *Measured* (Release, five interleaved
  rounds): no speed change — a 50-term polynomial 37.0 -> 36.9 ms, a mixed
  small-expression workload 48.2 -> 47.2 ms.

- [x] **`a + b` allocates about five times.** `Expr::add` takes a
  `std::vector<Expr>` by value (1), `normalize()` builds `flat` (2),
  `partitionNumbers` builds `numbers` and `rest` (3, 4), then
  `make_shared` (5), plus a `std::sort` over two elements. Given Boost is
  now a dependency and Boost.Container is *already being compiled* as part
  of it, `boost::container::small_vector<Expr, 4>` for operands and
  scratch is nearly free to adopt. A two-operand fast path in
  `operator+`/`operator*` (skip flatten/partition when neither side is the
  same kind and at most one is a number) would remove most of it.

  *Outcome:* done in `8908cb0`, without `small_vector`: Boost.Container is a
  compiled library, and linking it into this static one would have changed the
  installed package's dependencies. The normaliser now flattens only when an
  operand needs it and otherwise works in the caller's vector, partitions the
  numbers in place, and returns a lone number untouched instead of rebuilding
  it. *Measured:* `x + y` 5 -> 2 allocations, `x + 1` 8 -> 3, `(x + 1) + y`
  17 -> 7, the polynomial 1619 -> 830; Release, ten interleaved rounds, the
  polynomial 37.8 -> 30.6 ms and the mixed workload 49.0 -> 25.4 ms. A test
  written to pin reals folding "in any order" failed; checked against the old
  normaliser, chained `+` never had that property, so the test was corrected.

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

- [x] **`readFrame` is O(n²) on large replies.** Every 4 KB chunk appends
  to `buffer` and then `buffer.find(end)` searches *from the beginning*.
  Not yet visible in practice — `expand((x+y+z)^40)` is a 6 KB reply and
  takes 16 ms end to end — but it is quadratic in reply size by
  construction. Search from `max(0, oldSize - end.size())` instead, and
  raise the transport chunk size (64 KB) — the 4 KB buffer means a 1 MB
  reply is 256 syscalls and 256 searches.

  *Outcome:* done in `9130264`, both halves: the search starts just before the
  bytes that arrived, and a read takes up to 64 KB. *Measured:* `readFrame`
  alone over 4 KB reads, 4 MB went from 49-52 ms to 12-13 ms, now roughly
  linear; against Maxima, the 889 KB reply of `expand((x+y+z)^120)` from about
  51 to 46 ms, its transfer share from 15-17 ms to 12. Most of the rest is
  Lisp printing. None of this could be measured until the truncation above was
  fixed: before it, every large reply was capped near 7 KB.

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

- [x] **`Context` construction clears the reply cache just to read a
  name.** `evaluateOrThrow(kernel, "context")` goes through `evalTracked`,
  which clears the in-memory cache, before anything has changed. Use
  `evalPure` for the read.

  *Outcome:* no change needed. Since `6bc0f7d` the read is one step of the
  conversation that opens the context and records it with `remember()`, which
  clears the cache regardless — the new scope's statement is a change of
  state. A read that did not clear would change nothing observable, so no API
  was added for it.

- [x] **`evalNumeric`'s `walk()` allocates a `std::vector<double>` per
  function call**, and `isEvaluable` builds failure strings it then
  discards. Use a small stack array (max builtin arity is 2 except
  `max`/`min`) and pass `nullptr` for the failure sink.

  *Outcome:* done in `2df55b4`: an eight-slot stack array, spilling only for a
  longer call, and failure messages built only when there is a sink.
  *Measured* (Release, 200,000 calls): `evalNumeric` on an expression of
  function calls 4 -> 0 allocations and about 165 -> 87 ms; `isEvaluable` on an
  unbound symbol 42 -> 4 ms; on a relation, which printed the whole expression
  to discard the message, 45 allocations and about 520 ms -> 0 and 0.4 ms.

- [x] **`Compiled::pushConstant` dedups with `std::find` on `double ==`.**
  Merges `-0.0` with `0.0` (sign of zero lost — harmless in practice) and
  never merges NaN. Compare bit patterns if you want exact dedup; it is
  O(n) per constant either way, fine for expression sizes seen here.

  *Outcome:* fixed in `8bb6ca4`, and it was not harmless: `atan2(0.0, -1)` is
  pi and `atan2(-0.0, -1)` is -pi, so the compiled form of their sum answered
  2 pi where `evalNumeric` answered 0, and `1/0.0 + 1/-0.0` gave inf instead of
  NaN. A test comparing the two evaluators failed on both first. Constants now
  share a slot only when equal with the same sign, or both NaN.

## 4. API ergonomics

- [x] **No `operator<<` and no `std::formatter`** for `Expr`, `Integer`,
  `Symbol`. Every print in the demo is `.str()`. Ten lines, large quality
  of life gain. *(Now also the natural place to hang a format spec that
  selects a renderer: `std::format("{:tex}", e)`.)*

  *Outcome:* done in `2121eb4`. `operator<<` for all three, and a
  `std::formatter` for each: `{}` is `str()`, `{:tex}` and `{:mathml}` choose a
  renderer, and the usual string options follow, after a colon when a notation
  is given (`{:tex:>40}`). An unknown notation is a `std::format_error`, which
  for a constant format string is a compile error. The stream operators are
  defined out of line, so `<mx/expr.hpp>` does not pull in `<ostream>`.

- [x] **`Expr` has no ordering.** It cannot be a `std::map` key, cannot be
  sorted, cannot be put in a `std::set` — yet a total order already exists
  in `detail::compareExpr`. Expose it as `operator<=>` (fixing the NaN
  case first, §1).

  *Outcome:* done in `dda4dee`, deliberately not as `operator<=>` or `<`:
  `Expr` converts implicitly from numbers and symbols, so `x < 0` would compile
  and mean "sorts before" rather than build `lt(x, 0)`. Instead there is
  `mx::canonicalOrder`, a `std::weak_ordering` since 0.0 and -0.0 are equal but
  print differently; and `mx::CanonicalLess`, for sorting and for
  `std::set<Expr, CanonicalLess>`. (`std::less` was specialised for `Expr` and
  `Symbol` at first, so containers needed no comparator; removed after
  MinGW/clang 22 failed to compile it. libc++ 22's tree replaces `std::less<T>`
  with the transparent `std::less<>` whatever it is specialised to, and that
  calls `<`. A ten-line program shows the same with no library involved.)
  Two expressions are equivalent exactly when they are `==`, tested over a
  corpus. Making that hold found that numbers compared as doubles, so 2^100 and
  2^100 + 1 were equivalent; exact numbers are now compared exactly when their
  doubles tie. Comparing exactly throughout cost 23% on a 50-term polynomial;
  doubles first measured 31.3 -> 31.2 ms (Release, interleaved).

- [x] **No local structural substitution.** `subst(f, x, 5)` is a Maxima
  round trip for what is a tree rewrite. A `replace(expr, symbol, value)`
  in `src/core` — no kernel — would be the single most-used helper in any
  numeric-driver code, and it composes with `Compiled`.

  *Outcome:* done in `5bc2844`, as `mx::replace` in `<mx/traverse.hpp>`,
  beside `contains`, which moved there because it needs no kernel either;
  `<mx/ops.hpp>` includes the new header, so no caller changed. The result is
  rebuilt through the builders, so it is normalised — `3*x + 2` at x = 2 is 8 —
  but not evaluated: `sin(x)` at 0 is `sin(0)`, which is the difference from
  `subst`. Untouched subtrees are shared. Opaque text that mentions the symbol
  throws, rather than leaving the symbol silently behind.

- [x] **No traversal helpers.** `args()` is enough to write a recursion,
  but a `visit`/`transform`/`anyOf` would stop every caller writing the
  same one (`contains` and `mentionsSymbol` in this codebase are already
  the same function twice).

  *Outcome:* done in `8e77475`: `visit` (every node, pre-order), `anyOf` (stops
  at the first yes) and `transform` (bottom-up, rebuilding through the builders,
  sharing what it leaves alone). `contains`, `replace` and `mentionsSymbol` are
  each one of them now. Found: `transform` cannot use `==` to tell that an
  operand came back unchanged, because 0.0 == -0.0 and a rewrite from one to the
  other would be dropped; it compares representations instead, and a test pins
  that.

- [x] **The wire format leaks through `Kernel::eval`.** *(Partly done:
  `eval`/`evalPure`/`evalTracked` now have `const Expr &` overloads, so a
  caller can send structure. The reply is still raw text.)* It returns
  `Reply::value` as raw s-expression text. A public `Kernel::evalExpr`
  returning `std::expected<Expr, Failure>` — which is what `ops.cpp`'s
  private `evaluate()` already is — would let users who need a Maxima
  function this library has not wrapped get an `Expr` back without
  parsing s-expressions themselves. Keep `eval` for the raw case.

  *Outcome:* done in `a88c5e0`: `Kernel::evalExpr`, for text or an `Expr`,
  with `eval`'s cache semantics, and a free `mx::toExpr(const Reply &)`, so an
  `evalPure` or `evalTracked` reply reads the same way without a method for
  every combination. `Failure` moved to `<mx/reply.hpp>`. The operations and
  `Context` read their replies through `toExpr`, replacing two private copies of
  the parse.

- [x] **The operation set is thin for "basic workable".** Missing and
  cheap to add given the existing `evaluate()` helper: `is(...)`
  (ask Maxima a predicate under the current assumptions — the natural
  partner to `Context`), `taylor`, `trigsimp`/`trigexpand`/`radcan`,
  `sum`/`product`, `partfrac`, `float`/`numer` (Maxima-side numeric
  evaluation, complementing the local one), `lhs`/`rhs`, `coeff`,
  `nroots`/`realroots`/`find_root`, `ode2`. Matrices can wait; they need a
  typed node to be pleasant.

  *Outcome:* done in `51a3779`, all but matrices: `is` (answering
  `Truth::True`, `False` or `Unknown`), `taylor`, `trigsimp`, `trigexpand`,
  `radcan`, `partfrac`, `coeff`, `toFloat` (`float` being a C++ keyword; it
  covers `numer` too), `sum` and `product` (a `Failure` when no closed form is
  found), `nroots`, `realroots`, `findRoot` and `ode2`; `lhs` and `rhs` are
  local, and a `derivative` builder makes the `'diff` noun an ODE is written
  with. Found: a symbolic `sum` closes only under `simpsum`, which `ev` turns on
  for the one evaluation; `product` stays a noun even so; and `ode2` prints why
  it failed onto the pipe before answering `false`, which a test shows the
  session survives.

- [x] **`functions.hpp` puts `sin`, `cos`, `log`, `abs`, `exp`, `sqrt` in
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

  *Outcome:* done in `33e5f90`, by removing the hazard rather than documenting
  it: the builders are templates constrained to an `Expr` or a `Symbol`, so a
  call on a plain number has no mx candidate at all. Found: the builders were not
  the only case. `mx::abs` and `mx::gcd` on `Integer`, and `mx::pow`, offered
  the same implicit-constructor candidate; `abs` now takes exactly an `Integer`,
  and `gcd` and `pow` need at least one argument of their own type, so
  `pow(x, 2)` and `gcd(n, 1001)` still work. The one call relying on the old
  behaviour was `tour.cpp`'s `mx::sin(0)`. Compile-time checks pin all of it.
  Also done: builders for `tanh`, `asinh`, `acosh`, `atanh`, `erf`, `floor`,
  `ceiling` and `signum`, each round-tripped through Maxima in a test; `minf()`,
  with `minusInf()` deprecated; `%gamma` in the numeric layer; `%phi` and
  `%gamma` documented.

- [x] **`simplify` is `ratsimp`.** Documented, but the name promises more
  than it does; `ratsimp` as the public name (with a doc pointing at
  `trigsimp`, `radcan`) is more honest and matches Maxima's vocabulary,
  which the rest of `ops.hpp` already does.

  *Outcome:* done in `ecaa286`: `ratsimp` is the public name, documented as
  knowing no identities and pointing to `trigsimp`, `trigexpand` and `radcan`.
  `simplify` stays as its alias, not deprecated, so existing code is unaffected;
  the README and examples say `ratsimp`.

- [x] **`isUnevaluated(result, "list")` is used to mean "is a list".**
  A misnomer that reads as "Maxima failed" at every call site in `solve`.
  Add `isList()`.

  *Outcome:* done in `b71ccdf`; `solve` now calls `isList()`.

- [x] **`wrongKind()` prints the kind as an integer.** "expression is not
  an integer (kind 4)". Add a `to_string(Kind)` / `kindName()` — it is
  also wanted for tests and logging.

  *Outcome:* done in `498e495`: `mx::kindName`, with `operator<<` and a
  `std::formatter` for `Kind`; the message now reads "(its kind is Symbol)".
  `tour.cpp` carried its own copy of the same switch, which is gone.

- [x] **`Bindings` is keyed by `std::string`; `Compiled` takes
  `span<const Symbol>`.** Two spellings of "which symbol". Accepting
  `Symbol` in `Bindings` (or a transparent comparator over both) would
  make the numeric API read consistently.

  *Outcome:* done in `719436a`. `Bindings` is a small class over the same map,
  and an entry takes a `Symbol` or a name, so `{{x, 2.0}, {"y", 3.0}}` works and
  every existing `{{"x", 2.0}}` still compiles. Lookups are still by
  `string_view`, building nothing.

- [x] **Printing `-1*x` and `(-1*x)^2`.** `-(x+1)` prints `-1*(1 + x)`
  and `(-x)^2` prints `(-1*x)^2` (measured). Valid Maxima, but every
  user will read it as a bug. The printer already special-cases a leading
  `-1` inside sums; extend it to products at the top level and inside
  `Pow`.

  *Outcome:* resolved by the renderer layer in `666756c`, which carries a sign
  as a flag rather than a `-1` factor. `-(1 + x)` and `-x - y` are pinned in
  `test_render.cpp`, and `(-x)^2` is in its round-trip corpus.

- [x] **`x**2` and `1e400` are parse errors.** Maxima accepts `**` as
  `^`; accept it. `1e400` overflows `double` — either throw a clearer
  message ("out of range") or produce `inf`.

  *Outcome:* done in `e310a01`: `**` is `^` when the stars are adjacent, as in
  Maxima's lexer, and joins the corpus checked against Maxima's own parser.
  `1e400` is a `ParseError` saying it is out of the range of a double — refused
  rather than read as `inf`, which would be a different value.

## 5. Documentation drift

The comments are unusually good at saying *why*, which makes the stale ones
stand out. All refer to plan steps as future work that has since shipped:

- [x] `include/mx/expr.hpp`, `Expr::parse` doc: a paragraph is truncated
  mid-sentence ("…becomes an Opaque node holding its") and then
  contradicted by the next one. Delete the stale paragraph.
  *(Fixed in `3ea48d5`.)*
- [x] `include/mx/reply.hpp`: "Text only for now: PLAN.md step 7 adds the
  reader… Until then this is the rawest useful thing". *(Fixed.)*
- [x] `include/mx/kernel.hpp`: "This is the whole public surface for now…
  structured expressions arrive with the term layer (PLAN.md steps 7-9)".
  *(Fixed in `b71ccdf`.)*
- [x] `include/mx/ops.hpp`, `sharedKernel`: "Not thread-safe — see PLAN.md
  step 13". Wrong, see §2. *(Already fixed in `2a74041`, which rewrote the
  doc: starting it is thread-safe, and calls on it take turns.)*
- [x] `include/mx/context.hpp`: "so that PLAN.md step 14's cache key can
  include them". *(Fixed in `bddb2f0`. The step shipped differently: a change of
  assumptions clears the reply cache, and the persistent cache keys on the replay
  journal. The C++ copy only backs `assumptions()`.)*
- [x] `src/core/printer.cpp` header: "which is also what the Expr →
  Maxima direction of the translation layer will need (PLAN.md step 9)".
  *(Fixed — and now the opposite is true: the printer is not on the path to
  Maxima at all.)*
- [x] `src/transport/child_process_win32.cpp`: "Step 13 replaces this
  with a dedicated reader thread feeding a bounded queue". *(Gone with the file
  in `83f3bc8`, when Boost.Process replaced the hand-written transports.)*
- [x] `include/mx/config.hpp`, `timeout`: "Maxima keeps computing until it
  is killed" — true, but `recover()` *does* kill it on timeout; say so,
  since the current wording suggests a runaway process is left behind.
  *(Fixed in `6f5a2a7`, which also says every timeout pays for a startup.)*
- [x] `src/wire/from_maxima.cpp`: `mapInteger` and `mapRational` have
  `Opaque` fallbacks for "digits that do not parse" that can no longer
  happen — the lexer guarantees digits and `Integer` is unbounded. Dead
  code; remove or `assert`.

  *Outcome:* fixed in `97faf78`, and only half dead. `mapInteger`'s fallback
  could not be reached; `mapRational`'s could, by a denominator of zero or a
  part that is not an integer, and turned `(RAT 1 0)` into the text `(1/0)`.
  Both now throw `ParseError`, which `fromMaxima` promises for a term that is not
  well formed. Tests written first failed on the old code.
- [x] `src/kernel/persistent_cache.hpp`: the concurrency claim (see §1).
  *(Already fixed in `f2de237`, with the per-writer temporaries it describes.)*

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

- [x] ~~**`boost::container::small_vector` for `Node::args` and the
  normaliser's scratch vectors.** Already compiled as a transitive
  dependency of multiprecision. See §3.~~

  *Struck:* not recommended in the end. §3's allocation work (`8908cb0`)
  measured the gain without it, and Boost.Container is a compiled library:
  linking it into this static one would have added a dependency to the
  installed package.

- [x] **`std::variant` for the Node payload.** Standard library, no
  dependency. See §3.

  *Outcome:* done in `bfcdea6`, under §3.

- [x] ~~**A real database for the persistent cache?** SQLite would give
  bounded size (there is *no eviction* today — `Config::cacheDirectory`
  grows forever), atomic multi-entry writes and a proper cross-process
  story. It is a compiled dependency, though, and the one-file-per-entry
  design is genuinely simple.~~ Recommendation: keep the files, add a size
  cap with LRU-by-mtime eviction, and fix the temp-name collision (§1).

  *Outcome:* SQLite struck; the recommendation done. The temp-name collision
  was fixed in `f2de237`. The size cap is `e4dd610`: `Config::cacheDirectoryLimit`,
  256 MB by default. A read refreshes an entry's modification time. Over the
  limit, the oldest entries go until the directory is at three quarters of it,
  and temporaries over an hour old go too. The first write learns the
  directory's size, so a change of assumptions — which used to rebuild the
  cache object — now restamps it in place rather than rescanning.

- [x] **Keep hand-written:** the LRU cache (70 lines, nothing to gain),
  FNV-1a (stability across builds is the whole point; `std::hash` and
  `boost::hash` do not promise it), the s-expression reader (small,
  tested against golden files), and the Pratt parser (you asked for no
  third-party parser, and it is the right call — its bugs in §1 are
  semantic, not structural).

  *Outcome:* kept, as recommended. The fuzzing below is what the reader and the
  parser needed instead.

- [x] **A fuzz target for the two parsers.** libFuzzer/AFL on
  `parseSExpr` and `Expr::parse` is an afternoon and is how hand-written
  readers earn trust. `kMaxSExprDepth` exists, so someone already thought
  about hostile input; a fuzzer would find what the depth limit does not.

  *Outcome:* done in `377a397`, behind `-DMAXIMA_CPP_BUILD_FUZZERS=ON`: libFuzzer
  targets with AddressSanitizer where Clang has libFuzzer (Linux here; MinGW's
  Clang has none), replay programs elsewhere, and a corpus that runs as CTest
  tests. `fuzz_parser` also checks that what parses prints back to the same
  expression. It found six bugs, each fixed with a test and its input kept in
  the corpus:

  - `Expr::parse` had no depth limit, so deeply nested text overflowed the stack
    (`890bb5d`). The reader's `kMaxSExprDepth` had never had a counterpart. A
    count alone was not enough: clang-cl's and MSVC's Debug builds overflowed
    Windows' 1 MB stack before 1000 levels, so the parser also stops at 256 KB
    of stack used (`69c534a`).
  - `Integer::parse` handed zero-padded literals of more than eighteen digits
    to `cpp_int`, which reads a leading 0 as octal. With an 8 or 9 that threw
    an exception that escaped the reader; without, the value was silently wrong
    (`9715c42`).
  - `;` ended an atom but nothing consumed it, so the reply reader returned
    empty atoms at the same place until memory ran out (`71de9ac`).
  - The printer writes negative powers as divisions, which read back in a
    different shape: `x^-2` as `(x^2)^-1`, `2^-1` as the Rational, and
    `s/(n*f)` as a reciprocal of a product. The normaliser now keeps one form,
    as Maxima does (`0d16e20`).
  - A reciprocal of a number went below the line too, where it was multiplied
    into the coefficient's denominator and folded when read back: `4/269*0^-1`
    printed as `4/(269*0)`. It stays a power now (`db25b4b`).
  - `-0.0` is not below zero, so the renderer took it for positive, yet it
    prints with a minus: as a base, `(-0.0)^-1` printed as `-0.0^(-1)`, which
    reads back as `-(0.0^-1)`. The sign now comes from the sign bit (`8cd67fd`).

  A seventh, that a Real infinity prints as `inf`, which reads back as the
  symbol, was a question of design rather than a slip. It was left for a
  decision, then fixed as the last item of §1 describes (`98a69db`).

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
- [x] Out-of-order `Context` destruction. *(Already tested since `255e5d1`,
  with its §2 fix: "contexts ended out of order leave the survivors intact" in
  `test_context.cpp`, and a restart after an out-of-order end.)*
- [x] Two threads sharing one `Kernel` (the README promises it is safe;
  nothing exercises it). *(`dc0f03b`: four threads share one `Kernel` against
  Maxima, mixing a cached `evalPure` with a raw `eval` that clears the cache,
  every question distinct, and every answer comes back to its own caller. The
  session's locking was already tested over FakeTransport; this tests the
  promise itself.)*
- [x] `Kernel` after move. *(Already tested since `c1b91eb`, with its §2 fix:
  every call on a moved-from `Kernel` throws `KernelError`.)*
- [x] `mod`/`round` against Maxima's own answers — the numeric builtins
  are the one place the library computes something Maxima also computes,
  so cross-check them the way `test_integer.cpp` cross-checks bignums.
  *(`de0061e`: `test_numeric.cpp` checks 66 `mod` pairs and 12 `round` values
  against a live kernel. The other builtins are not yet cross-checked.)*
- [x] Parser: every Maxima operator the subset *claims* to reject should
  have a test that it does reject it, and `!!`, `**`, chained relations
  should be decided and pinned. *(`7fbed5a`: every family of Maxima syntax
  the subset leaves to `mx::parse` — assignment and definitions, quoting, `.`
  and `^^`, subscripts, `and`/`or`/`not`, `if` and `for`, the `;` and `$`
  terminators, the `?` Lisp escape — is a `ParseError`. The three decisions were
  already pinned: `!!` is the double factorial, `**` is `^` (`e310a01`), and a
  chained relation is refused unless parenthesised.)*

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
