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

Updated after `5f8d086`. Resolved findings are ticked where they stand, with
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
- **User-supplied renderers** (`666756c`). A type-erased `proxima::Renderer<T>` over
  a shared presentation layer, with `str()` reimplemented on top of it.
  Resolves §3's "printer runs the normaliser" and §4's `-1*x` printing. Found:
  `x - (1 + y)` printed as `x - 1 + y`, which re-parses as a different
  expression.
- **A regression from that, fixed** (`d058f70`). `666756c` printed
  `sqrt(1 - x^2)` as `1 - x^2^(1/2)` — again a different expression — because
  a missing `root()` was synthesised from already-rendered text. The demo
  found it; roots are now rewritten as display nodes and walked normally.
- **TeX and MathML renderers** (`666756c`, `693771a`): `to_tex()` and
  `to_mathml()`. Found: both padded small exponents, `10^{-07}`.
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
  `proxima::CanonicalLess`.
- **§5, one commit each** (`3ea48d5` … `edd45a1`). Three of the seven had
  already been fixed by earlier work and are ticked with those commits. Found
  on the way: the dead `Opaque` fallback in `map_rational` was not dead — a
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
- **§7, all but CI** (`91df296` … `5f8d086`); CI is left for later. Turning
  the warning flags on found three warnings the earlier GCC sweep had missed;
  clang-tidy's first run found 47 findings, the
  real ones fixed and the rest either excused in the file or left out of it.

Suite: 322 cases / 4734 assertions on Windows (GCC and clang-cl), 323 / 4732 on
Linux (222 when the review was written).

Every section is closed except §7's CI item, which is left for later.

A second full review, made after the rename to Proxima, is §9 at the end of
this file. It is where the open work now is. §9.1 is done except the two
items that wait on §9.6 — `eval_expr`'s `query` and the throwing operators —
and §9.2's parser fix is in (`3292b45` … `d7f32f8`). Suite after that round:
333 cases on Windows under GCC, clang-cl and MSVC, 335 on Linux. Then the
naming convention of §9.3: functions and variables are snake_case, types
PascalCase, and every identifier in this file was renamed with the code.
Then the first half of §9.6, in `f868659`: FXT is a dependency, every
operation returns `proxima::result<T>` — `fxt::result<T>` — with a `Cause`,
and the throwing half of the API is gone.

---

## 1. Correctness — wrong answers

These produce a result that disagrees with Maxima, silently.

- [x] **`mod` in the numeric evaluator is `std::fmod`; Maxima's `mod` is
  floored.** `eval_numeric(mod(-7, 3))` gives **-1**; Maxima gives **2**.
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
  overloads explicitly. Same hazard exists on `proxima::Integer`'s constructors
  for anything that later adds a floating constructor. (Measured.)

  *Outcome:* fixed in `aac36c1`, with both suggested fixes together. The trap
  was wider than described: the wide character types were never excluded, so
  `Expr(u'a')` was the Integer 97 and `Integer(u'7')` compiled and meant 55,
  and `x + true` quietly built `1.0 + x`. Two concepts in `integer.hpp` —
  `IntegralNumber` and `BooleanOrCharacter` — now decide it for both classes,
  keeping `std::int8_t` and `std::uint8_t` (spelled with `signed char` and
  `unsigned char`) as numbers. The deleted constructors make the error name
  the type: `use of deleted function 'proxima::Expr::Expr(T) [with T = bool]'`.

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

- [x] **NaN breaks `compare_expr`'s strict weak ordering.** `compare_expr`
  orders numbers through `double`; for NaN neither `<` nor `>` holds, so
  distinct NaNs compare *equal* to everything numeric, which violates the
  precondition of the `std::sort` calls in `normalize()` — that is
  undefined behaviour, not merely a wrong order. Also `Expr::real(NaN) ==
  Expr::real(NaN)` is false for two distinct nodes while their hashes are
  equal, and NaN prints as `nan`, which Maxima reads as a *symbol*. Decide:
  either reject NaN at `Expr::real` (throw) or give it a total order
  (`std::strong_order` on the bit pattern) and a spelling. (Measured.)

  *Partly done:* since `b71ccdf`, sending a NaN to Maxima throws `proxima::Error`
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
  `discover_maxima` do not. (`src/kernel/discovery.cpp`)

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
  §3's `read_frame`, whose large replies never grew.* Maxima runs with Lisp's
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
  `proxima::Error`; with no infinite Real left to be an operand, that is always an
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
  entered. No frame is produced, so `read_frame` waits the full
  `Config::timeout` (**two minutes** by default), throws `TimeoutError`,
  and then `recover()` kills and restarts the kernel — paying the
  transport's 2-second grace period on top. *Measured:* `eval("1$ 2")`,
  `eval("x; 3")` and `diff(Expr::opaque("x$ 0"), x)` each took the full
  timeout and threw `TimeoutError`; the session did come back healthy
  after each. The last one matters most: it reaches the stall through a
  *typed* public operation, not the raw escape hatch. Also measured:
  `eval("1) + cppsend(0, [7]")` succeeds — the spliced text is live
  Maxima code and can call the framing helper itself.

  This is reachable from: `Kernel::eval` / `eval_pure` with user text,
  any `Expr::opaque(...)`, any `Symbol` or `Expr::function` head containing
  a character Maxima's reader treats specially, and `proxima::parse` — no,
  `proxima::parse` is safe: it quotes the text into a string literal for
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
  kernel's life.** `state_accounted_ = false` is never reset. One diagnostic
  `eval("1+1")` and `Config::cache_directory` is dead until a new Kernel.
  The reasoning is sound (an unrecorded change cannot be keyed), but there
  is no `Kernel::eval_pure`-style way back and nothing tells the caller.
  Either expose a `resetPersistence()` / document loudly, or make `eval`
  restart the kernel with the journal replayed — which *does* restore an
  accounted state.

  *Outcome:* fixed in `cf6a0eb` with both halves, but not by making `eval`
  restart — that would discard exactly the definitions `eval` is used to
  make. `Kernel::persistence_active()` reports the state and `Kernel::restart()`
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

- [x] **`PersistentCache::read_field` trusts the stored length.** A corrupt
  or hostile `.reply` file with length `18446744073709551615` makes
  `text.resize()` throw `std::length_error` / `bad_alloc`, which escapes
  `eval_pure` as a non-`proxima::Error` exception. Cap the length (a reply cannot
  exceed the file size) and treat anything else as a miss.

  *Outcome:* fixed in `a2a55fd`, capping each field at what remains of the
  file. The test reproduced the `std::length_error` first.

- [x] **A moved-from `Kernel` is a null pointer waiting to be
  dereferenced.** Every method does `session_->…` unchecked. Either
  document "moved-from is unusable" or throw `KernelError`.

  *Outcome:* both, in `c1b91eb`: every method goes through one checked
  accessor that throws `KernelError`, and the move operations say so.

- [x] **`shared_kernel()` and static destruction order.** A function-local
  static Kernel is destroyed at exit; a user's own static that holds a
  `Context` (which holds a raw `Kernel*`) and outlives it will call into a
  dead object. Also: `ops.hpp` says `shared_kernel` is "Not thread-safe —
  see PLAN.md step 13", which is wrong on both counts (C++11 statics are
  thread-safe to initialise, and step 13 serialised the Kernel). Fix the
  comment; consider `Context` holding a `shared_ptr` or a weak reference.

  *Outcome:* fixed in `2a74041` with the weak reference. A Kernel owns a
  lifetime token that moves with its session and expires before the session
  is destroyed; a Context holds a `weak_ptr` to it, so once the Kernel is gone
  its operations throw `KernelError` and its destructor does nothing. That
  covers any Context outliving its Kernel, not only a static one outliving
  `shared_kernel()`. The comment is corrected.

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
  non-ASCII path — or a non-ASCII `Config::user_dir` — will fail or be
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
  short write is silently dropped; the next `read_frame` then times out
  with a misleading diagnosis.

  *Outcome:* fixed in `83f3bc8`. `asio::write` writes everything or reports
  an error, and an error marks the transport closed, so the next `receive()`
  and `alive()` report the dead child at once.

- [x] **The session mutex is held for the whole computation.**
  `cache_stats()` and `set_timeout()` take the same lock as `eval`, so both
  block for up to two minutes behind a running integral, and `set_timeout`
  cannot shorten an in-flight call. Separate a short lock for the
  bookkeeping from the long one for the pipe.

  *Outcome:* fixed in `539d93c` as suggested: a pipe lock for the conversation
  and a state lock for everything else, with `read_frame` re-reading the
  timeout each poll so `set_timeout` shortens a waiting call. Splitting them
  meant the journal can change mid-computation, so `eval_pure` now caches an
  answer only if a state generation counter is unchanged across the call.
  `eval_pure`'s cache lookups deliberately still take the pipe lock — see the
  next item.

- [x] **A Context's statement and its journal record are two separate
  calls.** `Context::assume` sends `assume(...)` through `eval_tracked`, then
  records it with `remember()`; `~Context` does `forget()`, then
  `killcontext`. Each is locked on its own, so another thread's `eval_pure` can
  run in between, compute under Maxima's new state, and file the answer under
  the journal's old one — in the in-memory cache, and in `cache_directory`,
  where it outlives the process. The window predates `539d93c` (the old single
  lock was also released between the two calls); the generation counter added
  there closes only the case where the journal changes *during* a
  computation. Fix: one session call that evaluates and records (or forgets
  and evaluates) under the pipe lock, so the change and its record are
  atomic. Found by reasoning while splitting the lock, not yet measured.

  *Outcome:* fixed in `6bc0f7d` as proposed. `MaximaSession::converse_atomically`
  runs its steps under one hold of the pipe lock, offering `eval_tracked`,
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

- [x] **`read_frame` is O(n²) on large replies.** Every 4 KB chunk appends
  to `buffer` and then `buffer.find(end)` searches *from the beginning*.
  Not yet visible in practice — `expand((x+y+z)^40)` is a 6 KB reply and
  takes 16 ms end to end — but it is quadratic in reply size by
  construction. Search from `max(0, oldSize - end.size())` instead, and
  raise the transport chunk size (64 KB) — the 4 KB buffer means a 1 MB
  reply is 256 syscalls and 256 searches.

  *Outcome:* done in `9130264`, both halves: the search starts just before the
  bytes that arrived, and a read takes up to 64 KB. *Measured:* `read_frame`
  alone over 4 KB reads, 4 MB went from 49-52 ms to 12-13 ms, now roughly
  linear; against Maxima, the 889 KB reply of `expand((x+y+z)^120)` from about
  51 to 46 ms, its transfer share from 15-17 ms to 12. Most of the rest is
  Lisp printing. None of this could be measured until the truncation above was
  fixed: before it, every large reply was capped near 7 KB.

- [x] **Win32 `receive` polls with `Sleep(1)`.** `PeekNamedPipe` +
  `Sleep(1)` in a loop; on a default Windows timer that sleep is 1–15 ms,
  so every round trip carries that latency floor. *Measured:* **15.5 ms
  per trivial `eval_pure`** (200 cache-missing `1+i` calls), which is
  almost exactly Windows' default 15.625 ms scheduler tick — the cost is
  the sleep, not Maxima. That is 3 s for 200 questions, and it is the
  number behind "a round trip costs milliseconds" in the docs. The comment
  says "Step 13 replaces this with a dedicated reader thread" — it did
  not. Options: overlapped I/O with an event, a reader thread feeding a
  condition variable, or Boost.Process's async pipes (§6).

  *Outcome:* fixed in `83f3bc8` with Asio's pipes, which wait on the
  completion port. Same 200-call measurement: **0.04 ms** per trivial
  `eval_pure`, and 1.5 ms per `expand` round trip (was 15.6 ms).

- [x] **`Context` construction clears the reply cache just to read a
  name.** `evaluate_or_throw(kernel, "context")` goes through `eval_tracked`,
  which clears the in-memory cache, before anything has changed. Use
  `eval_pure` for the read.

  *Outcome:* no change needed. Since `6bc0f7d` the read is one step of the
  conversation that opens the context and records it with `remember()`, which
  clears the cache regardless — the new scope's statement is a change of
  state. A read that did not clear would change nothing observable, so no API
  was added for it.

- [x] **`eval_numeric`'s `walk()` allocates a `std::vector<double>` per
  function call**, and `is_evaluable` builds failure strings it then
  discards. Use a small stack array (max builtin arity is 2 except
  `max`/`min`) and pass `nullptr` for the failure sink.

  *Outcome:* done in `2df55b4`: an eight-slot stack array, spilling only for a
  longer call, and failure messages built only when there is a sink.
  *Measured* (Release, 200,000 calls): `eval_numeric` on an expression of
  function calls 4 -> 0 allocations and about 165 -> 87 ms; `is_evaluable` on an
  unbound symbol 42 -> 4 ms; on a relation, which printed the whole expression
  to discard the message, 45 allocations and about 520 ms -> 0 and 0.4 ms.

- [x] **`Compiled::push_constant` dedups with `std::find` on `double ==`.**
  Merges `-0.0` with `0.0` (sign of zero lost — harmless in practice) and
  never merges NaN. Compare bit patterns if you want exact dedup; it is
  O(n) per constant either way, fine for expression sizes seen here.

  *Outcome:* fixed in `8bb6ca4`, and it was not harmless: `atan2(0.0, -1)` is
  pi and `atan2(-0.0, -1)` is -pi, so the compiled form of their sum answered
  2 pi where `eval_numeric` answered 0, and `1/0.0 + 1/-0.0` gave inf instead of
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
  defined out of line, so `<proxima/expr.hpp>` does not pull in `<ostream>`.

- [x] **`Expr` has no ordering.** It cannot be a `std::map` key, cannot be
  sorted, cannot be put in a `std::set` — yet a total order already exists
  in `detail::compare_expr`. Expose it as `operator<=>` (fixing the NaN
  case first, §1).

  *Outcome:* done in `dda4dee`, deliberately not as `operator<=>` or `<`:
  `Expr` converts implicitly from numbers and symbols, so `x < 0` would compile
  and mean "sorts before" rather than build `lt(x, 0)`. Instead there is
  `proxima::canonical_order`, a `std::weak_ordering` since 0.0 and -0.0 are equal but
  print differently; and `proxima::CanonicalLess`, for sorting and for
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

  *Outcome:* done in `5bc2844`, as `proxima::replace` in `<proxima/traverse.hpp>`,
  beside `contains`, which moved there because it needs no kernel either;
  `<proxima/ops.hpp>` includes the new header, so no caller changed. The result is
  rebuilt through the builders, so it is normalised — `3*x + 2` at x = 2 is 8 —
  but not evaluated: `sin(x)` at 0 is `sin(0)`, which is the difference from
  `subst`. Untouched subtrees are shared. Opaque text that mentions the symbol
  throws, rather than leaving the symbol silently behind.

- [x] **No traversal helpers.** `args()` is enough to write a recursion,
  but a `visit`/`transform`/`any_of` would stop every caller writing the
  same one (`contains` and `mentions_symbol` in this codebase are already
  the same function twice).

  *Outcome:* done in `8e77475`: `visit` (every node, pre-order), `any_of` (stops
  at the first yes) and `transform` (bottom-up, rebuilding through the builders,
  sharing what it leaves alone). `contains`, `replace` and `mentions_symbol` are
  each one of them now. Found: `transform` cannot use `==` to tell that an
  operand came back unchanged, because 0.0 == -0.0 and a rewrite from one to the
  other would be dropped; it compares representations instead, and a test pins
  that.

- [x] **The wire format leaks through `Kernel::eval`.** *(Partly done:
  `eval`/`eval_pure`/`eval_tracked` now have `const Expr &` overloads, so a
  caller can send structure. The reply is still raw text.)* It returns
  `Reply::value` as raw s-expression text. A public `Kernel::eval_expr`
  returning `std::expected<Expr, Failure>` — which is what `ops.cpp`'s
  private `evaluate()` already is — would let users who need a Maxima
  function this library has not wrapped get an `Expr` back without
  parsing s-expressions themselves. Keep `eval` for the raw case.

  *Outcome:* done in `a88c5e0`: `Kernel::eval_expr`, for text or an `Expr`,
  with `eval`'s cache semantics, and a free `proxima::to_expr(const Reply &)`, so an
  `eval_pure` or `eval_tracked` reply reads the same way without a method for
  every combination. `Failure` moved to `<proxima/reply.hpp>`. The operations and
  `Context` read their replies through `to_expr`, replacing two private copies of
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
  `radcan`, `partfrac`, `coeff`, `to_float` (`float` being a C++ keyword; it
  covers `numer` too), `sum` and `product` (a `Failure` when no closed form is
  found), `nroots`, `realroots`, `find_root` and `ode2`; `lhs` and `rhs` are
  local, and a `derivative` builder makes the `'diff` noun an ODE is written
  with. Found: a symbolic `sum` closes only under `simpsum`, which `ev` turns on
  for the one evaluation; `product` stays a noun even so; and `ode2` prints why
  it failed onto the pipe before answering `false`, which a test shows the
  session survives.

- [x] **`functions.hpp` puts `sin`, `cos`, `log`, `abs`, `exp`, `sqrt` in
  `namespace proxima`.** Under `using namespace proxima;` with `<cmath>` in scope,
  `abs(x)` for an `int x` now has a viable `proxima::abs(Expr)` candidate via
  the implicit constructor; overload resolution still picks the
  `int`/`double` one, but it is the kind of thing that turns into an
  ambiguity the day someone adds an overload. Consider a sub-namespace
  (`proxima::fn`) or accept it and document "don't `using namespace proxima`".
  Also: `minus_inf()` vs `inf()` naming; `tanh`, `asinh`, `acosh`, `atanh`,
  `erf`, `floor`, `ceiling`, `signum` are in the numeric builtin table but
  have no builder; `%gamma` is a Maxima constant the numeric layer does
  not know (`std::numbers::egamma` exists); `%phi` is known but
  undocumented.

  *Outcome:* done in `33e5f90`, by removing the hazard rather than documenting
  it: the builders are templates constrained to an `Expr` or a `Symbol`, so a
  call on a plain number has no Proxima candidate at all. Found: the builders were not
  the only case. `proxima::abs` and `proxima::gcd` on `Integer`, and `proxima::pow`, offered
  the same implicit-constructor candidate; `abs` now takes exactly an `Integer`,
  and `gcd` and `pow` need at least one argument of their own type, so
  `pow(x, 2)` and `gcd(n, 1001)` still work. The one call relying on the old
  behaviour was `tour.cpp`'s `proxima::sin(0)`. Compile-time checks pin all of it.
  Also done: builders for `tanh`, `asinh`, `acosh`, `atanh`, `erf`, `floor`,
  `ceiling` and `signum`, each round-tripped through Maxima in a test; `minf()`,
  with `minus_inf()` deprecated; `%gamma` in the numeric layer; `%phi` and
  `%gamma` documented.

- [x] **`simplify` is `ratsimp`.** Documented, but the name promises more
  than it does; `ratsimp` as the public name (with a doc pointing at
  `trigsimp`, `radcan`) is more honest and matches Maxima's vocabulary,
  which the rest of `ops.hpp` already does.

  *Outcome:* done in `ecaa286`: `ratsimp` is the public name, documented as
  knowing no identities and pointing to `trigsimp`, `trigexpand` and `radcan`.
  `simplify` stays as its alias, not deprecated, so existing code is unaffected;
  the README and examples say `ratsimp`.

- [x] **`is_unevaluated(result, "list")` is used to mean "is a list".**
  A misnomer that reads as "Maxima failed" at every call site in `solve`.
  Add `is_list()`.

  *Outcome:* done in `b71ccdf`; `solve` now calls `is_list()`.

- [x] **`wrong_kind()` prints the kind as an integer.** "expression is not
  an integer (kind 4)". Add a `to_string(Kind)` / `kind_name()` — it is
  also wanted for tests and logging.

  *Outcome:* done in `498e495`: `proxima::kind_name`, with `operator<<` and a
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

- [x] `include/proxima/expr.hpp`, `Expr::parse` doc: a paragraph is truncated
  mid-sentence ("…becomes an Opaque node holding its") and then
  contradicted by the next one. Delete the stale paragraph.
  *(Fixed in `3ea48d5`.)*
- [x] `include/proxima/reply.hpp`: "Text only for now: PLAN.md step 7 adds the
  reader… Until then this is the rawest useful thing". *(Fixed.)*
- [x] `include/proxima/kernel.hpp`: "This is the whole public surface for now…
  structured expressions arrive with the term layer (PLAN.md steps 7-9)".
  *(Fixed in `b71ccdf`.)*
- [x] `include/proxima/ops.hpp`, `shared_kernel`: "Not thread-safe — see PLAN.md
  step 13". Wrong, see §2. *(Already fixed in `2a74041`, which rewrote the
  doc: starting it is thread-safe, and calls on it take turns.)*
- [x] `include/proxima/context.hpp`: "so that PLAN.md step 14's cache key can
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
- [x] `include/proxima/config.hpp`, `timeout`: "Maxima keeps computing until it
  is killed" — true, but `recover()` *does* kill it on timeout; say so,
  since the current wording suggests a runaway process is left behind.
  *(Fixed in `6f5a2a7`, which also says every timeout pays for a startup.)*
- [x] `src/wire/from_maxima.cpp`: `map_integer` and `map_rational` have
  `Opaque` fallbacks for "digits that do not parse" that can no longer
  happen — the lexer guarantees digits and `Integer` is unbounded. Dead
  code; remove or `assert`.

  *Outcome:* fixed in `97faf78`, and only half dead. `map_integer`'s fallback
  could not be reached; `map_rational`'s could, by a denominator of zero or a
  part that is not an integer, and turned `(RAT 1 0)` into the text `(1/0)`.
  Both now throw `ParseError`, which `from_maxima` promises for a term that is not
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
  bounded size (there is *no eviction* today — `Config::cache_directory`
  grows forever), atomic multi-entry writes and a proper cross-process
  story. It is a compiled dependency, though, and the one-file-per-entry
  design is genuinely simple.~~ Recommendation: keep the files, add a size
  cap with LRU-by-mtime eviction, and fix the temp-name collision (§1).

  *Outcome:* SQLite struck; the recommendation done. The temp-name collision
  was fixed in `f2de237`. The size cap is `e4dd610`: `Config::cache_directory_limit`,
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
  `parse_sexpr` and `Expr::parse` is an afternoon and is how hand-written
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

- [x] **No `LICENSE` file.** README has a "Licence" section about
  Maxima's GPL and the process boundary, but this library's own licence
  is never stated anywhere. That is the first thing a consumer looks for.

  *Outcome:* done in `91df296`: MIT, Copyright (c) 2026 Kenneth Balslev, and the
  README's licence section now says so before explaining Maxima's GPL.

- [x] **`.idea/` is tracked** (7 files). The `.gitignore` only excludes a
  subset. Either commit the whole project config deliberately or ignore
  the directory.

  *Outcome:* ignored, in `2dba191`. The files record one machine's toolchains
  and paths, and CLion regenerates them from `CMakeLists.txt`; they are
  untracked, and local copies are untouched.

- [x] **Warnings are clean but nothing enforces it.** Add
  `-Wall -Wextra -Wpedantic -Wshadow -Wconversion` (`/W4` for MSVC) to the
  library target, and `-Werror` in CI. The sweep for this review found
  zero, so it costs nothing to turn on now and it will catch the first
  regression.

  *Outcome:* done in `980947c`, on every target of this project's own:
  `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
  -Wold-style-cast` for GCC and Clang, `/W4` for MSVC and clang-cl, and
  `-Werror` or `/WX` behind `PROXIMA_WARNINGS_AS_ERRORS`, off by default for
  a CI to turn on. It was not quite free: it found three warnings the earlier
  GCC sweep had missed. MSVC's C4459, a local `version` hiding `proxima::version`;
  GCC's `-Wcomment`, a `//` line of ASCII art ending in a backslash, in a test
  the sweep had not compiled; and a sign conversion that only Linux's
  `uint64_t`, `unsigned long` there, raises. All four compilers now build it
  with no warning in this project's code.

- [ ] **No CI.** Three toolchains were verified by hand for the Boost
  change (GCC/Windows, clang-cl/Windows, GCC/Linux). A GitHub Actions
  matrix running `ctest -LE maxima` (no Maxima needed) on all three, plus
  one job with Maxima installed for the integration suite, would make
  that automatic. The LP64 `long long` ambiguity that only Linux caught is
  the argument.

- [x] **No `.clang-format` / `.clang-tidy`.** The code is consistently
  styled, which means a format file already exists in someone's head;
  commit it.

  *Outcome:* both committed. `.clang-format` (`e1473e7`) is the best of 24
  variants measured against the tree: LLVM, 4-space indentation, breaks before
  binary operators, 85 columns. It still differs on about one line in eight,
  mostly hand-placed breaks, so it is not applied wholesale; the file says to
  format changed lines with `git clang-format`. `.clang-tidy` (`5f8d086`) holds
  the code to bugprone, performance and a few other checks, and it passes them.
  The first run found 47 findings. The real ones are fixed: four `std::move`s
  into const references, seven one-character string appends, four discarded
  asio return values, two `int` products widened to sizes, two parameters taken
  by value for no reason, and a decrement inside a loop condition. Two intended
  designs carry a `NOLINT` saying why, and five checks whose every finding was
  intended are left out, each named with its reason in the file.

- [x] **`std::getenv` triggers MSVC's deprecation warning** under
  clang-cl (seen in the build log). `_CRT_SECURE_NO_WARNINGS` on the
  target, or `_dupenv_s` under `_WIN32`.

  *Outcome:* already gone. `39dad91`, which made non-ASCII paths work, reads the
  environment through the wide API on Windows, and no `std::getenv` call is left.
  Neither clang-cl's nor MSVC's build shows the warning.

- [x] **`BUILD_SHARED_LIBS OFF` is passed to Boost through CPM
  `OPTIONS`.** CPM sets those as cache variables, so it also pins *this*
  project's default. Harmless today (the library is static regardless),
  but it will surprise whoever adds a shared-library option.

  *Outcome:* fixed in `b3010ea`: a normal variable set around `CPMAddPackage` and
  unset after, so Boost still builds static and a user's value is back in force
  afterwards. A fresh configure no longer caches `BUILD_SHARED_LIBS`.

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
  Maxima, mixing a cached `eval_pure` with a raw `eval` that clears the cache,
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
  the subset leaves to `proxima::parse` — assignment and definitions, quoting, `.`
  and `^^`, subscripts, `and`/`or`/`not`, `if` and `for`, the `;` and `$`
  terminators, the `?` Lisp escape — is a `ParseError`. The three decisions were
  already pinned: `!!` is the double factorial, `**` is `^` (`e310a01`), and a
  chained relation is refused unless parenthesised.)*

---

## Not problems

Things I checked that are fine and that a reviewer might flag anyway.

- `shared_kernel()` as a function-local static is thread-safe to
  construct; the comment is wrong, not the code.
- `operator/` producing `x*0^(-1)` for division by zero: deliberate,
  Maxima is the one that objects, and it does.
- `x - x` staying `x - x`: correct, one canonicaliser.
- `-0.0 == 0.0` as `Expr`: true, with equal hashes; consistent.
- `Integer`'s small-value fast paths: verified allocation-free.
- `isupper`/`islower` in `decode_maxima_name` are locale-sensitive in
  principle; Maxima symbol names reaching here are ASCII in practice.
- The 4 KB transport chunk and `find`-from-zero are the same issue as
  §3's `read_frame`; listed once.

---

## 9. Second review — the project as it stands after §1–§8 and the rename

A second full read, made after every earlier section was closed and the
project became Proxima: all 19,755 lines — the sixteen public headers, every
source file, the tests, the fuzz targets, the build — with four things *run*
rather than reasoned about, each marked *measured* where it appears: the
compile cost of the public headers, the cost of building a long sum term by
term, whether Maxima accepts every `Feature`, and whether Maxima reads
`maxima-init.mac` from the directory the library points it at.

**Overall.** The first review's verdict holds and has strengthened. Layers
only point downward; the wire is structure in both directions; there is one
canonicaliser; the suite is 322 cases on Windows and 323 on Linux across four
compilers with no warnings; the two hand-written parsers are fuzzed. What is
left is different in kind from §1–§8. Those were defects in the small. What
remains is the *shape* of the API: half of it reports failure as a value and
half throws; the meaning of an operation depends on ambient state (the shared
kernel, whichever `Context` objects happen to be alive on any thread); the
public header carries Boost into every consumer; and two hot paths are
superlinear. And there is one security hole and one wrong enumerator, both
found by running Maxima rather than reading it.

The request this time was also for a functional-programming direction, in the
style of FXT. §9.6 is the answer: not a list of nits but a proposed re-shaping
of the public surface, ordered so that each step is useful on its own. The
short version: Proxima's *core* is already functional — `Expr` is an immutable
value with structural equality, everything under `src/core` is pure, and the
kernel is the one effect boundary. What is not functional is the *interface*
to the kernel. That is where the work is.

### 9.1 Correctness and safety

- [x] **`Feature::Prime` is not a Maxima feature.** *Measured:* against
  Maxima 5.50.0, `declare(zz, prime)` fails with `declare: unknown property
  prime`; every other enumerator is accepted (each on a fresh symbol, so the
  result is not a conflict between opposites). Maxima's own `features` list
  is `integer, noninteger, even, odd, rational, irrational, real, imaginary,
  complex, analytic, increasing, decreasing, oddfun, evenfun, posfun,
  constant, commutative, lassociative, rassociative, symmetric,
  antisymmetric, integervalued`. So `Context::declare(p, Feature::Prime)`
  throws `MaximaError`, and the README advertises it. Remove `Prime`; add the
  missing ten if they are wanted; and add the test that would have caught
  this — declare every enumerator against a live kernel — since no test does.

  *Outcome:* fixed in `3292b45`. `Prime` is gone and the ten missing features
  are added, so `Feature` is exactly Maxima's list, grouped by what they
  describe (values, functions, operators). The new test declares each on a
  symbol of its own and checks `featurep`; a probe first confirmed that all
  twenty-two report true.

- [x] **The default user directory is a code-injection path on a shared
  Unix machine.** *Measured:* Maxima loads `maxima-init.mac` from
  `$MAXIMA_USERDIR` (a probe with `proxima_probe: 42$` in that file printed 42;
  with the variable unset it printed the unbound symbol). With
  `Config::load_user_init` false — the default — the library sets
  `MAXIMA_USERDIR` to `temp_directory_path()/proxima/userdir`, which on Linux
  and macOS is `/tmp/proxima/userdir`: one directory for every user of the
  machine. Whoever creates it first owns it; a `maxima-init.mac` placed there
  is executed by every Proxima process of every other user, with that user's
  privileges. The setting whose purpose is "load *nothing* of the user's" loads
  whatever another user chose. Windows is unaffected in the common case, since
  `%TEMP%` is per user. Fix: a directory private to the process — `mkdtemp`
  under the temp directory, mode 0700, removed when the kernel ends — so no
  init file can exist there at all; or, if a stable directory is wanted for
  what Maxima writes into it, a per-user one under `$XDG_CACHE_HOME` /
  `%LOCALAPPDATA%` with its ownership and mode checked before use. Then a test
  that the default is private. (`Config::cache_directory` has the same trust
  shape: anyone who can write to a shared cache directory can make
  `integrate` answer anything, since entries carry no integrity check. That
  one is opt-in and documented as shared, so it is a documentation item:
  say that the directory must be private to the trust domain.)

  *Outcome:* fixed in `fb16850`, with the second of the two fixes, not the
  first: a per-user directory, `<tmp>/proxima-<uid>/userdir`, rather than one
  per process, so nothing is left behind in the temporary directory by every
  run. It is made with `mkdir(0700)` — never a `chmod` after, which would
  leave a moment when it is open — and an existing one is accepted only if
  `lstat` shows a directory, not a link, owned by this user and closed to
  everyone else; otherwise the kernel refuses to start and says why. Windows
  keeps `%TEMP%\proxima\userdir`. Tests: the default's owner and mode, and
  refusal of an open directory, a symbolic link and a file (Unix only). A
  `Config::user_dir` given explicitly is used as it is, and its documentation
  now says it must not be writable by others. `Config::cache_directory`'s
  documentation says the same, in the terms above, in the commit that ticks
  this section.

- [x] **A reply that contains the frame delimiter truncates its own frame.**
  Not measured. `read_frame` searches for `@@E<id>@@` anywhere in the stream,
  and the value is printed with `~s`, so a Maxima *string* whose text contains
  the delimiter — reachable from `Expr::opaque("\"@@E7@@\"")`, or from any
  string a user builds — ends the frame early. The truncated value then fails
  `parse_sexpr`, which surfaces as a `ParseError` escaping `to_expr`; the tail of
  the real frame is skipped by the next request. Ids are sequential, so the
  id is guessable. Low severity, one-line fix: put a per-session random nonce
  in the delimiters (`@@E<nonce>-<id>@@`), or have the helper length-prefix the
  value. The nonce is the smaller change.

  *Outcome:* fixed in `fb16850` with the nonce: each session draws a 64-bit
  key from `std::random_device` and every delimiter carries `<key>-<id>`. The
  tag reaches the Lisp helper as a Maxima string, which it prints with `~a`,
  so the helper itself is unchanged. The two test-only constructors take the
  key, so scripted replies can still be written before the session exists.
  New tests: a value containing its own request's old-style delimiter
  survives, and two keys differ.

- [x] **`read_frame`'s buffer is unbounded.** A child that streams without
  ever completing a frame is bounded only by `Config::timeout`, and at pipe
  speed two minutes is gigabytes. Cap the buffer (256 MB, say) and treat
  reaching it as a `KernelError` with a terminate, like a timeout.

  *Outcome:* fixed in `fb16850`: 256 MB, `MaximaSession::kMaxFrameBytes`. A
  `KernelError`, so `converse` restarts the child as it does for any broken
  conversation. Tested with two half-limit chunks and no closing delimiter.

- [ ] **`Kernel::eval_expr` has `eval`'s destructive semantics.** It calls
  `eval`, so it clears the reply cache and switches persistence off for the
  kernel — while being the convenient entry point a user reaches for to ask a
  question (`eval_expr("gcd(12, 18)")`, the README's own example). The safe
  verb is the long one (`eval_pure`), the short one is the dangerous one, and
  nothing in the type says which is which. §9.6 item 6 makes the distinction
  a type. Until then: add `query(text) -> std::expected<Expr, Failure>` on the
  pure path, and document `eval_expr` as a statement.

  *Outcome:* half done. `eval_expr` is now documented as a statement, in bold,
  in `kernel.hpp` (`2ac8654`) and in the README (`495fbb0`), both pointing to
  `to_expr(kernel.eval_pure(...))` for a pure question. `query` is not added:
  it would be a fifth evaluation verb that §9.6 item 6 would remove again, so
  it waits for that decision. Since `f868659` the pure question is a one-liner
  anyway — `kernel.eval_pure(form) | fxt::and_then(to_expr)` — which takes
  most of the sting out; the verb itself still waits for item 6.

- [x] **The in-memory reply cache is bounded by count, not bytes.**
  `Config::cache_entries` is 4096; a single reply can be 889 KB
  (`expand((x+y+z)^120)`, §3). Worst case 3.6 GB resident. The persistent cache
  already has a byte budget; give `ReplyCache` one too, or evict by size.

  *Outcome:* fixed in `495fbb0`. A new `Config::cache_bytes`, 64 MB, enforced
  alongside `cache_entries`: least recently used first. Each entry is charged
  its text, the key twice (the index holds a copy), and a fixed 256 bytes
  for the nodes. A reply larger than the limit on its own is not kept, and
  takes any older answer under its key with it, so a stale reply is never
  served in place of one that replaced it.

- [ ] **Arithmetic operators can throw.** Since `98a69db`, `Expr(1e308) *
  Expr(10.0)` throws `proxima::Error` from `operator*`, and so does any
  builder, `transform`, `replace` or `Expr::parse` that folds such numbers.
  Decided deliberately (§1, the infinity item), and it does match Maxima —
  but a value type whose `+` throws for a value reason is a trap in generic
  code and in every pipeline §9.6 proposes. Two consistent alternatives, both
  in the spirit of the `x/0` precedent ("let Maxima be the one to object"):
  leave the overflowing fold *unfolded*, so `1e308 * 10.0` stays a product of
  two reals — which costs uniqueness of canonical form for that one corner; or
  keep the throw but make it a distinct `OverflowError` so callers can tell it
  from misuse, and say so on `operator+`/`operator*`. Recommendation: the
  second now, the first if the pipeline work makes throwing operators hurt.

  *Outcome:* deliberately not done in this round. Which of the two it should
  be depends on §9.6 item 1 (one result type everywhere), so it is left open
  with that.

  *Outcome, with §9.6 item 1 (`f868659`):* the second alternative. The
  operators and builders throw `proxima::OverflowError`, distinct from
  `Error`, and `Expr::parse` — which can return a value — reports the same
  fold as `Cause::Overflow`. The fold stays folded, so canonical form keeps
  its uniqueness.

  *Outcome, with §9.6 item 1 (`f868659`):* the second alternative. The
  operators and builders throw `proxima::OverflowError`, distinct from
  `Error`, and `Expr::parse` — which can return a value — reports the same
  fold as `Cause::Overflow`. The fold stays folded, so canonical form keeps
  its uniqueness.

- [x] **Persistent-cache hits write to disk.** With a limit set — the
  default — `find()` refreshes the entry's mtime on every hit, a metadata
  write. It is once per entry per process (the in-memory cache takes over),
  so it is acceptable; document it, and consider refreshing only when the
  mtime is older than, say, an hour.

  *Outcome:* fixed in `495fbb0` as suggested: a hit refreshes the time only
  when the recorded use is an hour old or more. The recency test now ages its
  entries in hours; a new test checks that a read within the hour leaves the
  file's time alone.

- [x] **`derivative(f, variable, order)` takes the variable as `const Expr
  &`** where every other calculus function takes `Symbol` for exactly the
  reason `symbol.hpp` gives. `derivative(y, x * 2)` compiles. Make it `Symbol`.

  *Outcome:* fixed in `93a1a00`. No caller passed anything but a symbol.

- [x] **`std::hash<Symbol>` is missing** although `std::hash<Expr>` exists
  and `Symbol` has `==`; `std::unordered_set<Symbol>` does not compile.

  *Outcome:* fixed in `93a1a00`, hashing as the expression, so it agrees with
  `std::hash<Expr>`; tested.

- [x] **`find_root`'s failure message formats the interval with
  `std::to_string(double)`**, which prints six decimals: a failure between
  1e-9 and 1e-8 reads "between 0.000000 and 0.000000". Use `std::format`.

  *Outcome:* fixed in `93a1a00`; the message now reads "between 1e-09 and
  1e-08", which a test pins.

- [x] **`to_tex` renders `minf` as `-\infty` inside a sum term**, so `x + minf`
  is `x + -\infty`; `minf` should carry its sign through `negate()`, as the
  MathML renderer does with `mrow`. Also a decision rather than a bug: the TeX
  and MathML renderers turn a user's own symbol named `gamma`, `pi`, `phi` or
  `mu` into the Greek letter, which is right for a physics formula and wrong
  for a variable that happens to be called `mu`. Say so in the docs, or limit
  the table to Maxima's `%`-constants.

  *Outcome:* fixed in `2ac8654`, and not quite as suggested: the claim that
  MathML already got it right was wrong — it produced `a + −∞` too. `minf`
  cannot become a negated `inf` in the shared presentation layer, because the
  text printer uses that layer and `a - inf` reads back as a different
  expression. So each typeset renderer folds it in its `sum`: `a + minf` is
  `a - \infty`, and `a - minf` is `a + \infty`. The Greek letters are
  documented in `tex.hpp` and `mathml.hpp`, and kept.

### 9.2 Performance

- [ ] **Building a sum term by term is far worse than linear.** *Measured*
  (Debug library, `-O2` probe, GCC 13, Windows):

  | terms | chained `operator+` | `Expr::parse` of the text | one `Expr::add` |
  |---:|---:|---:|---:|
  | 500 | 79 ms | 80 ms | 0.3 ms |
  | 1000 | 300 ms | 312 ms | 1.1 ms |
  | 2000 | 2349 ms | 2556 ms | 1.6 ms |
  | 4000 | 8961 ms | 8470 ms | 3.9 ms |

  Every `operator+` re-normalises the whole growing sum: flatten (a copy of
  all n operands), partition, sort, rebuild, one node allocated per step —
  O(n² log n) over the chain, and the parser folds its `+` run the same way,
  one operator at a time. Three fixes, in order of value: the Pratt loop
  already holds a run of same-precedence operands, so the parser should
  collect the run and build it with one `Expr::add`/`Expr::mul` (that alone is
  the 2000× at n=4000, and it is where the §6 stack-depth test noticed the
  cost); `operator+` on an `Add` and a leaf can insert at the sorted position
  instead of re-sorting; and the documentation should name `Expr::add(span)`
  as the way to build a long sum, with a `proxima::sum(range)` helper. A
  `std::accumulate` over 4000 terms is the natural thing to write and takes
  nine seconds today.

  *Outcome:* the first and third fixes are in `d7f32f8`. The parser collects a
  run of `+`/`-`, or of `*`/`/`, and builds it once; the same probe now
  measures `Expr::parse` at 0.7, 1.8, 3.1 and 6.9 ms for 500 to 4000 terms,
  down from 80 ms to 8.5 s. New tests parse 20,000-term sums and products and
  check runs against the chained operators they replace. `Expr::add`,
  `Expr::mul` and the arithmetic operators now say to build long sums with
  `add`. Still open: chained `operator+` (unchanged, 8.6 s at 4000 — each step
  builds a new immutable sum, so an insertion only removes the sort, not the
  copy), and a `proxima::sum(range)` helper, which belongs with the §9.6
  naming decision.

- [x] **Including `<proxima/expr.hpp>` costs 1.46 s and 191,590 preprocessed
  lines per translation unit.** *Measured* (GCC 13, the project's Debug flags,
  `-fsyntax-only`, best of three): the standard headers it needs cost 0.59 s
  and 72,776 lines; `<proxima/integer.hpp>` alone is 1.45 s and 191,308 lines.
  The difference — 0.85 s and 118,000 lines — is Boost.Multiprecision, which
  `proxima::Integer` holds by value and so puts in every consumer's every
  translation unit, and which therefore has to be *installed* beside the
  library (`BOOST_SKIP_INSTALL_RULES OFF`, the versioned `include/boost-1_92`,
  and the package config's `find_dependency`). §3's note called this "the
  whole price"; it is the largest cost a consumer of this library sees, and it
  is avoidable. Hide the backend behind an opaque inline buffer:
  `alignas(std::max_align_t) std::byte storage_[N]` in the header, with
  `static_assert(sizeof(cpp_int) <= N)` and the placement in `integer.cpp` —
  or the small-object layout the fast paths already assume, an `int64_t`
  inline and a pointer to a heap `cpp_int` only when it does not fit. Then no
  Boost header is public, the install rules go back to the usual `ON`, the
  package config needs only the private Boost.Process, and a consumer's
  compile time roughly halves. `cpp_rational` (§9.4) can hide behind the same
  wall.

  *Outcome:* done in `2c44c76`, with the second layout: an inline `int64_t`
  and a `shared_ptr<const detail::BigInt>` set only when the value does not
  fit. The public header declares `BigInt` and never defines it, so a shared
  pointer to it copies, moves and destroys with no Boost in sight, and
  copying a large value copies a pointer. The representation is canonical, so
  small values compare inline. Measured the same way: `<proxima/expr.hpp>` is
  now 86,440 lines and 0.74 s (from 191,590 and 1.46 s), `<proxima/ops.hpp>`
  102,072 and 0.89 s (from 203,653 and 1.61 s) — about 0.04 s above the
  standard headers alone. Runtime improved too, since two small values now
  meet machine arithmetic with an overflow check instead of cpp_int: Release,
  best of fifteen, small arithmetic 4.0× faster, small rational expressions
  1.4×, parsing 1.35×, large values unchanged within noise. One part of the
  prediction was wrong: the install rules cannot go back to `ON`. Proxima is
  static and links Boost.Process's compiled library, so consumers still need
  that installed. What changed is that `Boost::multiprecision` is linked as
  `$<BUILD_INTERFACE:...>`, so the exported target and the package config ask
  only for Boost.Process, and a consumer built against a clean install has no
  Boost directory on its compile line (checked). A new test includes every
  public header and fails to compile if any brings in Boost.

- [ ] **`transform` allocates a vector for every compound node even when
  nothing under it changes.** For a rewrite that touches one leaf of a large
  tree, that is one allocation per ancestor *and* per untouched sibling
  subtree. Delay the allocation until the first changed child, copying the
  prefix then. Also: `replace` and `contains` revisit shared subtrees, which a
  memo keyed by node identity would avoid for DAG-shaped expressions. Low.

- [ ] **`Expr` copies are atomic reference-count operations.** The
  normaliser and the traversals copy operands freely, and `std::shared_ptr`'s
  count is atomic. `boost::intrusive_ptr` (Boost is already here) with a
  non-atomic count, or a policy, would shave the constant; measure before
  changing — §3 showed single runs are noise. Low.

- [ ] **`Expr::str()` builds a display tree on every call.** A `DisplayNode`
  is an `Integer`, a `std::string`, two vectors and a `double` — some 150
  bytes and up to four allocations per node — built and discarded per call.
  Fine for diagnostics, which is what it is for; say so, and keep it out of
  hot loops. Caching the string in the node (`mutable`, once) is possible but
  not obviously worth the size. Low.

- [ ] **Small things in the persistent cache:** `read_field` does three seeks
  per field (twelve per entry read) to bound the length — read the file into a
  string once; `sweep()` sorts every entry when only the eviction boundary
  matters (`nth_element`). Low.

### 9.3 Ergonomics and the shape of the API

- [ ] **Three verbs for evaluation, and the safe one has the longest name.**
  `eval` (clears the cache, stops persistence), `eval_pure`, `eval_tracked`,
  `eval_expr` (as `eval`), plus `remember`/`forget` to maintain the journal by
  hand. The distinction between them is a promise the caller makes and
  nothing checks: `eval_pure("a: 7")` is accepted and quietly poisons every
  later cached answer. §9.6 item 6 makes the promise a type. Failing that,
  rename so the default is the safe one — `query` for the pure path, `execute`
  for a statement — and make `eval_tracked`, `remember` and `forget` internal
  once `Context` is their only client.

- [x] **`Reply` is a hand-rolled `expected` in a public header** — `ok`,
  `value`, `reason`. It should be `std::expected<std::string, Failure>` if the
  raw wire form stays public at all, and the wire form is better made
  `detail`: the public API deals in `Expr`, and `to_expr` already exists.

  *Outcome:* done in `f868659`. `Kernel::eval`, `eval_pure` and
  `eval_tracked` return `result<std::string>`; `to_expr` takes the wire text,
  so `kernel.eval_pure(form) | fxt::and_then(to_expr)` is how every
  operation reads its answer. `Reply` is `detail::Reply` in
  `src/kernel/reply.hpp`, the shape the two caches store, with
  `detail::to_result` as the one conversion.

  *Outcome:* done in `f868659`. `Kernel::eval`, `eval_pure` and
  `eval_tracked` return `result<std::string>`; `to_expr` takes the wire text,
  so `kernel.eval_pure(form) | fxt::and_then(to_expr)` is how every
  operation reads its answer. `Reply` is `detail::Reply` in
  `src/kernel/reply.hpp`, the shape the two caches store, with
  `detail::to_result` as the one conversion.

- [x] **Half the operations throw and half return `std::expected`.**
  `diff`, `expand`, `factor`, `ratsimp`, `subst`, `taylor`, `trigsimp`,
  `trigexpand`, `radcan`, `partfrac`, `to_float`, `coeff`, `nroots`,
  `realroots` and `is` throw `MaximaError`; `integrate`, `limit`, `solve`,
  `ode2`, `sum`, `product`, `find_root` and `parse` return a `Failure`. The rule
  — "no ordinary way to fail" — is a judgement made per function, not a
  property of the type: `diff(Expr::opaque("$"), x)` throws where
  `integrate` of the same input returns a value. Every caller who wants one
  handling style has to know the list. §9.6 item 1 proposes one shape.

  *Outcome:* done in `f868659`; see §9.6 items 1, 2 and 10.

  *Outcome:* done in `f868659`; see §9.6 items 1, 2 and 10.

- [ ] **`Expr(x)` everywhere.** The README, the tour and the tests write
  `pow(proxima::Expr(x), 2)` and `gt(proxima::Expr(n), proxima::Expr(0))` where
  `pow(x, 2)`, `x * x + 3 * x + 2` and `gt(n, 0)` all compile: `Symbol`
  converts implicitly and `pow` accepts it. The documentation is teaching the
  verbose spelling. Show the terse one, and put `namespace px = proxima;` in
  the README's first example.

- [x] **Accessors that throw on the wrong kind.** `integer_value()`, `name()`,
  `real_value()`, `relation_op()`, `opaque_text()` and `arg(i)` each throw
  `proxima::Error` unless the caller has checked `is(Kind::…)` first, so every
  reader of a tree is a `switch` on `kind()` followed by calls that could
  throw if the switch is wrong. `Node` is a `std::variant` already; §9.6
  item 4 exposes that as a `match` with typed views and `std::optional`
  accessors, which is what C++23 makes pleasant.

  *Outcome:* answered by §9.6 item 4 in `05f6d32`. The throwing accessors
  remain, documented as the form for code that has already checked the kind.

- [ ] **`Bindings::set` mutates, in an otherwise value-oriented numeric
  API.** Add `with(symbol, value)` returning a new `Bindings`, and consider
  `std::flat_map` (C++23; libstdc++ 15, MSVC 19.4x, libc++ 20) as the store —
  a handful of entries in contiguous memory beats a node-per-entry
  `std::map`.

- [ ] **`as_function` returns a `std::function`** — an allocation and an
  indirect call per point — when `Compiled` is already a callable value that
  can be returned by value. Return `Compiled`, or `auto`; if a type-erased
  form is wanted, `std::move_only_function`.

- [ ] **`shared_kernel()` is ambient global state that every operation
  defaults to.** Convenient for a script; for a library built on Proxima it
  means one Maxima for everyone, static-destruction order to reason about
  (§2 fixed the crash, not the design), and no way to tell from a call site
  which kernel it uses. Keep the default — it is what makes the README's
  first example three lines — but make the explicit form the one the
  documentation leads with, and see §9.6 item 3 for a `with_kernel` adaptor.

- [ ] **`Context` is ambient too, and process-global.** Which Maxima context
  is *current* is state in the Maxima process, so the meaning of
  `integrate(f, x)` depends on which `Context` objects are alive on any
  thread at that moment; two threads each holding a `Context` on one kernel
  interleave. The registry, the out-of-order teardown, the
  exception-swallowing destructor and the journal all exist to manage that
  ambient state. §9.6 item 5 replaces it with a value.

- [x] **Naming: camelCase against snake_case.** Proxima's free functions and
  members are camelCase — `evalNumeric`, `isEvaluable`, `toTeX`,
  `canonicalOrder` — while FXT and the standard library are snake_case —
  `and_then`, `value_or`, `transform`. The moment the two are used in one
  pipeline the seam shows: `integrate(f, x) | fxt::and_then(diffBy(x)) |
  fxt::value_or(zero)`. This has to be decided *before* the FP work in §9.6,
  because renaming afterwards is churn across every file. Recommendation:
  snake_case for functions and members, PascalCase for types — the standard
  library's convention and FXT's.

  *Outcome:* decided and done: snake_case for functions, methods, variables,
  parameters and members (private ones with a trailing underscore);
  PascalCase for types, concepts, template parameters and enumerators; named
  constants keep `kPascalCase`. 375 identifiers were renamed across the
  sources, the examples, the fuzzers and the Markdown — `eval_pure`,
  `to_tex`, `to_mathml`, `parse_sexpr`, `Config::cache_entries`. The rename
  set was taken only from identifiers that occur in C++ code outside strings
  and comments, so test data such as the Maxima symbol `xY` is untouched,
  while comments and messages naming a function follow it. Two helpers in
  `discovery.cpp` became `directory_exists` and `regular_file_exists`, since
  `std::filesystem` has the snake_case names and a path argument would have
  found them by argument-dependent lookup. `.clang-tidy` now enforces the
  convention with `readability-identifier-naming`, with four exceptions it
  documents; it reports nothing across `src`, `include`, `tests`, `examples`
  and `fuzz`. Names in the historical text of this file and `PLAN.md` were
  updated with the code, so they still name things that exist; names of
  things since removed (`evalRaw`, `resetPersistence`) are left as they were.

- [ ] **`Kernel::remember` and `forget` are public.** Manipulating the replay
  journal by hand is an invitation to make the journal lie, which is the one
  thing the persistent cache cannot survive. Make them `detail` (see also
  §9.6 item 6, where they disappear).

- [ ] **README, PLAN and TODO are 400, 1,400 and 1,100 lines of narrative.**
  The *why* is unusually well recorded, which is the reason this review could
  be done from the outside at all — but a newcomer wanting *how* has to read
  past it. Move the history into `docs/decisions/`, one file per decision in
  ADR style, and cut the README to the first two hundred lines. Process item.

### 9.4 What existing libraries could provide

- [x] **`boost::multiprecision::cpp_rational` for `Fraction` and the `Exact`
  accumulator.** `Expr::rational` reduces by the gcd and moves the sign by
  hand; `Exact::add`, `multiply` and `reduce` in the normaliser re-implement
  rational arithmetic. `cpp_rational` is exactly that — canonical, reduced,
  sign on the numerator — and is already in the Boost that is fetched. It
  removes some sixty lines and two places where gcd or sign handling could
  drift apart. Do it together with the opaque buffer in §9.2, so it stays out
  of the public header.

  *Outcome:* half, in `2c44c76`. The normaliser's `Exact` is now a
  `cpp_rational`, reached through the internal `src/core/big_int.hpp`, and
  its `add`, `multiply` and `reduce` are gone; its double conversion is now
  correctly rounded once, where dividing two doubles rounded twice and gave
  NaN when both parts overflowed. A 400-term sum of fractions got 5.3× faster.
  `Fraction` and `Expr::rational` keep their `Integer` fields: the node's
  payload is the public representation of a Rational, and routing every
  small rational through `cpp_rational` to reduce it would cost more than
  the gcd and two divisions it replaces, now that those are machine
  arithmetic. So there is one hand reduction left, not two.

- [ ] **`std::generator` (C++23) for traversal.** `for (const Expr &node :
  proxima::nodes(e))` reads better than a callback and composes with ranges:
  `any_of(e, p)` becomes `std::ranges::any_of(nodes(e), p)`. GCC 14 and MSVC
  19.39 have it; check libc++ 22 before relying on it, since clang is a
  supported compiler here.

- [ ] **`std::ranges::to` and views (C++23)** where the code loops by hand to
  build a vector: `map_arguments` is `form.items() | views::drop(1) |
  views::transform(from_maxima) | ranges::to<std::vector>()`; `solve`'s
  by-name collection, `sorted_children`, `Compiled`'s variable names likewise.
  Readability, not speed.

- [ ] **`std::print` / `std::println` (C++23)** in the examples and the tour
  in place of iostream; the library already formats with `std::format`.

- [ ] **`std::flat_map` (C++23)** for `Bindings`, as above.

- [x] **FXT itself.** Header-only, MIT, the same author, `std::expected`
  underneath by default. Its `operator|` is constrained on `expected_like`,
  which `std::expected<Expr, Failure>` satisfies (checked against
  `IsExpected.hpp`), so `proxima::integrate(f, x) | fxt::transform(to_tex)`
  works *today* with no change to either library. What does not interoperate
  is `fxt::attempt` and `fxt::result<T>`, which fix the error type to
  `fxt::failure`. Whether Proxima should *depend* on FXT or merely compose
  with it is §9.6 item 11.

  *Outcome:* a dependency, decided by its author; see §9.6 item 11.

- [ ] **A property-testing library** — RapidCheck, or Catch2's generators if
  the suite ever moved — for §9.5's invariants. A sixty-line generator over
  `Kind` inside the existing doctest suite would do as well.

- [ ] **Keep hand-written, as before:** the Pratt parser, the s-expression
  reader, the LRU. And one candidate for *removal* rather than replacement:
  §9.5, the render vtable.

### 9.5 Build, tests, process

- [ ] **CI** is the one §7 item still open, and the by-hand cycle used
  throughout this work — GCC and clang-cl on Windows, MSVC, GCC on Linux,
  `clang-tidy`, the fuzz corpora — is exactly the matrix to automate.

- [x] **Property tests for the invariants the design rests on.** Every
  guarantee below is asserted on hand-picked cases and none on generated
  ones: normalisation is idempotent (`Expr::add(e.args()) == e` for every
  `Add`); `a == b` ⇔ `canonical_order(a, b) == 0` ⇔ `a.hash() == b.hash()`;
  `Expr::parse(e.str()) == e` (the fuzzer checks this, but only when it
  runs); `transform(e, identity)` shares every node; and
  `from_maxima(parse_sexpr(to_maxima(e))) == e` needs no kernel and is never
  checked over random trees. A generator over `Kind` with a size bound, and
  a few hundred trees per run.

  *Outcome:* done in `301b59e`, `tests/test_properties.cpp`: all five of
  these and three more — `contains` agrees with a search of the tree,
  replacing a symbol by itself changes nothing, and every renderer accepts
  every tree. One correction to the statement above: equal hashes do not
  imply equality, so the check is `a == b` ⇔ `canonical_order(a, b) == 0`,
  and `a == b` ⇒ equal hashes. Idempotence is checked on every node kind,
  not only `Add`, by rebuilding the whole tree from its parts. No library
  code needed changing: 20,000 trees per property on three random seeds
  found nothing. That is only worth something if the properties can fail,
  so three deliberate bugs were made and reverted in turn, and each was
  caught by exactly the property aimed at it. One of them — an ordering that
  trusts doubles, the §1 bug — is only reachable with integers that round to
  the same double, so the generator draws neighbours of 2^53, 2^64 and
  2^100 on purpose, and a further test pins the generator's reach so the
  properties cannot quietly become vacuous. 400 trees by default (0.2 s);
  `PROXIMA_PROPERTY_CASES` and `PROXIMA_PROPERTY_SEED` widen a run.

- [x] **Every `Feature` against a live kernel** — the test that would have
  caught `Prime`. And once the user directory is fixed, a test that the
  default is private.

  *Outcome:* both done with the §9.1 fixes: "Maxima accepts every Feature"
  in `3292b45`, and the private-directory tests in `fb16850`.

- [ ] **The render vtable serves no use the code has.** `Renderer<T>` is 300
  lines of hand-written vtable and small-buffer storage so that one type can
  hold any renderer — but `render(expr, R &&)` erases the renderer, walks,
  and discards it, and nothing stores a `Renderer<T>` or keeps several in a
  container. The walk pays an indirect call per node for a capability with no
  caller. A template walk over the concrete `R` is simpler, faster, and loses
  nothing until a real use for heterogeneous storage appears — at which point
  `Renderer<T>` can wrap the template. A decision rather than a bug; the
  concepts and the display layer stay exactly as they are.

- [ ] **Apply `.clang-format` wholesale when §9.6 lands.** It was committed
  as "closest, not applied" (§7) to protect line history; a rewrite that
  touches most files anyway is the moment to reformat once and stop the
  drift.

- [ ] **PLAN.md says "Genuinely undecided: nothing."** It should now list the
  decisions this section asks for — naming, values versus exceptions,
  assumptions as values, the opaque `Integer` — so that the plan and the TODO
  agree about what is open.

### 9.6 A functional shape for the API, with FXT as the model

What is already functional, and should be said so in the documentation as the
library's purity boundary: `Expr` is an immutable value with structural
equality and a hash fixed at construction; every header in the core —
`expr`, `integer`, `symbol`, `functions`, `traverse`, `numeric`, `render`,
`tex`, `mathml` — is pure, with no I/O and no state beyond constants;
canonical form makes equality meaningful, which is what makes `Expr` a value
rather than a handle; and the kernel is the one place effects happen. That is
the right foundation and none of it needs to change.

What is not functional is the interface *to* the kernel, and a few habits
around the edges: failure is a value in half the operations and an exception
in the other half; three kinds of ambient state decide what an operation
means (the shared kernel, the live `Context` objects, and Maxima's own
"current context"); mutation is offered where a value would do
(`Bindings::set`, the `eval` verbs as unchecked promises, `remember`/`forget`);
traversal is callback-shaped; errors are untyped strings; and nothing composes
— there is no pipe, no partial application, no way to write a pipeline without
naming an intermediate at every step. Each item below fixes one of those, and
they are ordered so that each is useful without the next.

- [x] **1. One result type, everywhere.**

  ```cpp
  template <class T> using result = std::expected<T, Failure>;
  result<Expr> diff(const Expr &, const Symbol &, unsigned = 1, Kernel & = shared());
  ```

  Every kernel-backed operation returns `result<Expr>`; the throwing half of
  §9.3 goes. Callers who want the throwing style write `.value()` — which
  throws `std::bad_expected_access` — or `proxima::unwrap(r)`, which throws
  `MaximaError` carrying the message, and that becomes the only place
  `MaximaError` is thrown. The "no ordinary way to fail" distinction survives
  as documentation ("diff fails only for malformed input"), which is where a
  judgement belongs. Local, kernel-free operations (`replace` on an Opaque
  that mentions the symbol, the accessors) get the same treatment where they
  fail for a value reason, and keep throwing only for programming errors
  (`arg(i)` out of range).

  *Outcome:* done in `f868659`. Every operation in `ops.hpp` returns
  `result<T>` — `diff`, `expand`, `factor`, `ratsimp`, `simplify`, `subst`,
  `taylor`, `trigsimp`, `trigexpand`, `radcan`, `partfrac`, `to_float`,
  `coeff`, `is` (`result<Truth>`), `nroots` (`result<std::size_t>`) and
  `realroots` joined the ones that already did — and `evaluate_or_throw` is
  gone from `ops.cpp`. `proxima::unwrap(r)` is the throwing style, and the
  only place `MaximaError`, `ParseError`, `EvalError` and `OverflowError`
  are thrown for an ordinary outcome; `Context::assume` and `declare` keep
  throwing, being statements, until item 5. Not done, on purpose: the
  accessors (`integer_value()`, `arg(i)`) still throw, being programming
  errors, and item 4 is their answer. In the tests the change is mostly a
  `*` in front of the call.

- [x] **2. A `Failure` worth matching on, compatible with `fxt::failure`.**
  Today `Failure` is a string. A caller cannot tell "no closed form" from
  "Maxima needs an assumption" from "the kernel died" without parsing the
  message — and the third is the one they must handle differently. Give it a
  cause and, for the assumption case, the fact Maxima asked about as a
  value:

  ```cpp
  enum class Cause { NoClosedForm, NotSolved, MaximaError, NeedsAssumption,
                     Kernel, Timeout, Parse, Eval, Overflow };
  struct Failure {
      Cause cause;
      std::string message;
      std::optional<Expr> missing_fact;   // NeedsAssumption: the relation asked about
      std::exception_ptr exception;      // when wrapping one
  };
  ```

  With `Cause`, `fxt::ensure`-style gates and `fxt::match` branches read as
  intent rather than string searches, and `stuck.error().missing_fact` is what
  the README's "when Maxima needs a fact" section wants to show. The shape is
  deliberately `fxt::failure`'s — message, exception, typed context — so a
  bridge (item 11) is a conversion, not a redesign.

  *Outcome:* done in `f868659`, and more simply than proposed: `Failure` *is*
  `fxt::failure`, and the `Cause` rides as its `std::any` context —
  `fail(cause, message)` attaches it, `cause_of(failure)` reads it back,
  `Cause::Unknown` for a failure made elsewhere. The enumerators are
  `MaximaError`, `NeedsAssumption`, `NoClosedForm`, `NotSolved`, `NoLimit`,
  `UnexpectedAnswer`, `Parse`, `Eval`, `Argument`, `Overflow`, with
  `to_string` for logs. `NeedsAssumption` is recognised where the reply
  becomes a result, from the Lisp helper's own wording, so no caller has to.
  `missing_fact` is not there: the question is text Maxima composed, and
  parsing it back into a relation would be a guess; the message carries it.

- [x] ~~**3. Pipe adaptors for every operation.**~~ Each operation gains an
  overload without its subject that returns a closure, and `Expr` and
  `result<Expr>` both pipe into it — the plain value applies, the result
  short-circuits (Kleisli composition, which is what `fxt::and_then` does):

  ```cpp
  using namespace proxima::pipes;
  const auto F = f | diff(x) | expand() | integrate(x);        // result<Expr>
  const auto s = F | fxt::transform(proxima::to_tex) | fxt::value_or("?");
  const auto g = f | with_kernel(k) | diff(x) | factor();       // an explicit kernel
  ```

  `fxt::curry` does not fit these functions directly — default arguments and
  overloads defeat arity detection — which is why FXT's own operations are
  adaptors too, and why Proxima should follow that pattern rather than
  currying. The adaptor types live in `proxima` so that ADL finds the pipe;
  a `result<Expr> | adaptor` overload in `proxima` avoids requiring FXT for
  the common chain, and FXT's `and_then` works on it regardless.

  *Outcome:* struck, at FXT's author's suggestion, once the result type was
  FXT's (item 11). Everything this asked for except the spelling already
  works with FXT's own adaptors: `f | fxt::and_then(FXT_LIFT(expand))` for
  a single-argument operation, a lambda or `std::bind_back(FXT_LIFT(diff),
  x)` for one that takes a variable — `FXT_LIFT` is variadic, so the
  defaulted `order` and `kernel` parameters cost nothing. A second,
  Proxima-specific closure per operation would duplicate FXT's pipe
  machinery for `f | diff(x)` over `f | and_then(bind_back(LIFT(diff), x))`.
  The README's *Composing with FXT* section documents the idioms, and a
  test pins them. The one piece left open, deliberately, is an `operator|`
  that lets a bare `Expr` start a chain — FXT's pipe is constrained on
  expected-like types, so a chain starts from an operation's result or from
  `result<Expr>{f}` — to be added only if that wrapping proves common.

- [x] **4. `match` over expressions, and optional accessors.** `Node` is a
  `std::variant` already; expose that as a visit over cheap, non-owning
  views, with `fxt::overload` or deducing-this doing the dispatch:

  ```cpp
  const auto s = e.match(
      [](const Integer &n)            { return n.to_string(); },
      [](const proxima::Rational &q)   { return q.numerator.to_string() + "/" + ...; },
      [](double r)                    { return std::format("{}", r); },
      [](const proxima::SymbolView &v) { return std::string(v.name); },
      [](const proxima::Sum &s)        { return join(s.terms()); },   // and Product, Power, Call, Relation, Opaque
  );
  const std::optional<Integer> n = e.as_integer();     // replaces the throwing integer_value()
  n.transform([](const Integer &i) { ... });           // C++23 monadic optional
  ```

  The `switch (kind())` + throwing-accessor pairs throughout `numeric.cpp`,
  `render.cpp`, `to_maxima.cpp` and the tests become total matches the
  compiler checks. The shared immutable representation is untouched; the
  views are references into it.

  *Outcome:* done in `05f6d32`. `Expr::match(handlers...)` combines the
  handlers with `fxt::overload` and dispatches on the kind; a `requires`
  clause (`detail::HandlesEveryKind`) makes a set that misses a kind fail to
  compile, which the tests check with a concept rather than by hand. The
  views are `proxima::node::{Integer, Rational, Real, Symbol, Sum, Product,
  Power, Call, Relation, Opaque}`, uniform structs of references and spans
  into the node, so nothing is copied — cheaper than `integer_value()`,
  which returns an `Integer` by value. The result is the common type of the
  handlers' results, `void` included. The optional accessors are
  `as_integer`, `as_fraction` (any exact number, an Integer as n/1),
  `as_real` and `as_symbol`, owning their values so they are safe to keep;
  `Fraction` moved from `detail` into the public API for that. Two
  departures from the sketch: the views live in `proxima::node`, since
  `Symbol` and `Integer` were taken, and the throwing accessors stay, as the
  short form for code that has checked `is(Kind::...)`. `to_maxima`'s
  encoder is the first reader rewritten as a match; the other kind
  switches (`numeric.cpp`, `render.cpp`, `normalize.cpp`, `from_maxima.cpp`)
  are left as they are, being correct and the hottest paths in the library.
  A new property rebuilds every random tree through `match` and checks it
  equals the one the accessors rebuild, and that each optional accessor
  answers exactly when `kind()` says it should.

- [ ] **5. Assumptions as values, not scopes.** The largest change and the
  largest win. An `Assumptions` value is an immutable, ordered set of
  relations and declarations — structural equality, hashable, built with
  `assuming(gt(n, 0)).and(declared(n, Feature::Integer))`. Operations take
  one, defaulting to empty:

  ```cpp
  const auto A = proxima::assuming(gt(n, 0));
  const auto r = integrate(pow(x, n), x, A);                   // or:  f | with(A) | integrate(x)
  ```

  The kernel materialises a Maxima context per distinct `Assumptions` value
  on first use and keeps a small LRU of them, switches with `context:` under
  the pipe lock before each call (a microsecond), and keys the reply cache
  and the persistent stamp on the `Assumptions` hash — which makes the
  cache key *explicit* rather than "whatever the journal holds". After a
  restart, contexts are simply recreated on demand; the journal, `remember`,
  `forget`, the process-wide registry, out-of-order teardown, and the
  destructor that swallows exceptions all go, because there is no longer any
  ambient state to keep in step. The meaning of `integrate(f, x, A)` is a
  function of its arguments, on every thread, whatever else is alive. This is
  the reader-monad shape: the environment is passed in, not looked up.
  `Context` can survive as sugar over it for callers who like RAII, but it
  would no longer be the mechanism.

- [ ] **6. Queries and statements as types.** The four `eval` verbs and the
  unchecked purity promise collapse into two types and two methods:

  ```cpp
  result<Expr> Kernel::ask(const Query &);        // pure: cached, persisted
  result<unit> Kernel::tell(const Statement &);   // an effect: clears what it must
  ```

  `Query::form(expr)` and `Query::text("gcd(12, 18)")` are the pure escape
  hatches by construction; `Statement::text("a: 7")` is the only thing that
  invalidates a cache, and it says so in its name. `eval_expr`'s trap (§9.1)
  cannot be written. `fxt::unit` is the natural success type for `tell`.

- [ ] **7. Traversal as folds and ranges.** `visit`, `any_of` and `transform`
  are three special cases of one catamorphism:

  ```cpp
  template <class Algebra> auto fold(const Expr &e, Algebra &&alg);   // alg(node, folded children)
  Expr rewrite(const Expr &e, F &&f);   // f: const Expr& -> std::optional<Expr>; nullopt keeps the node
  std::generator<const Expr &> nodes(const Expr &e);
  ```

  `transform` becomes `fold` with `with_operands`; returning `std::optional`
  from the rewrite callback says "unchanged" directly, retiring the
  representation-identity trick that `transform` needs today; and `nodes(e)`
  makes `any_of` a `std::ranges::any_of`. All pure, all in the core.

- [ ] **8. Immutability where it is missing.** `Bindings::with(...)` beside
  `set`, or instead of it; `Config` is already a value copied into the
  kernel; `Compiled` is already immutable. `fxt::immutable<T>` is the right
  wrapper to show in the examples for a `Bindings` built up and then frozen.

- [ ] **9. Effects at the edge, stated.** A short table in the README: these
  headers are pure, these have effects, and `Kernel` is the only type with
  state. The code already honours it; the documentation should promise it,
  because it is the property that makes the rest of this section possible.

- [x] **10. Errors from the parser and the numeric layer as values too.**
  `Expr::parse` returns `result<Expr>` (with `Cause::Parse` and the offset);
  `Compiled`'s constructor cannot return a value, so add `compile(expr,
  vars) -> result<Compiled>` and keep the constructor for those who want the
  throw. `eval_numeric` → `result<double>` with `Cause::Eval`; `is_evaluable`
  stays as the cheap predicate.

  *Outcome:* done in `f868659`. `Expr::parse` returns `result<Expr>` with
  `Cause::Parse` (the offset in the message) or `Cause::Overflow`; the
  recursive-descent parser still throws internally and the boundary
  converts. `eval_numeric` returns `result<double>`; `compile(expr, vars)`
  is the value-returning form of `Compiled`'s constructor, which keeps its
  throw as documented. The parser fuzzer now treats *any* exception as a
  finding.

- [x] **11. Depend on FXT, or mirror it?** Two honest options. (a) Depend:
  `proxima::result` *is* `fxt::result`, `Failure` *is* `fxt::failure` with
  the `Cause` as context, and the adaptors are `fxt::and_then` and friends
  rather than a second implementation — at the price of a header-only
  dependency (plus its optional `tl::` fallbacks) in every consumer.
  (b) Compose: keep `std::expected<Expr, Failure>`, which FXT's pipe already
  accepts, and add one small header, `<proxima/fxt.hpp>`, that converts a
  Proxima `Failure` to an `fxt::failure` carrying the `Cause` as context, so
  `fxt::attempt` chains and `fxt::result` interoperate. Recommendation: (b)
  first — it keeps Proxima dependency-light and costs one header — and move
  to (a) only if the adaptor layer in item 3 would otherwise duplicate FXT's
  pipe machinery.

  *Outcome:* (a), decided by FXT's author, in `f868659`. FXT is fetched by
  CPM at a pinned commit, `DOWNLOAD_ONLY`, with an interface target declared
  in this project's CMake rather than FXT's own (which fetches a second CPM
  and the TartanLlama fallbacks a C++23 build does not need); `fixed_string`
  comes along so `<fxt.hpp>` compiles for a consumer. The public headers
  include only `fxt/monads/Expected.hpp` and `fxt/utils/Failure.hpp`, and
  `result.hpp` refuses `FXT_USE_TL_EXPECTED`, which would make the result a
  different type from the one the library was compiled with. FXT's headers
  are installed beside Proxima's, and a consumer built against a clean
  install composes a Proxima result with `fxt::and_then` with no FXT of its
  own (checked). Two things found on the way, for FXT: `Failure.hpp` streams
  a `string_view` without including `<ostream>`, which MSVC's STL refuses
  unless `<ostream>` came first — `result.hpp` includes it first, with a
  note; and `<fxt.hpp>` needs deducing this (`Match.hpp`) and
  `std::forward_like`, so GCC 13 cannot take the umbrella, only the headers
  it uses. And a third, found once the repository was public: the file is
  tracked as `utils/failure.hpp` while FXT's own headers include
  `Failure.hpp` — invisible on Windows, fatal on Linux. Proxima includes the
  tracked name; `<fxt.hpp>` and `Attempt.hpp` stay broken on Linux until FXT
  renames one or the other.

- [ ] **12. What not to do.** Do not make `Expr` a public `std::variant`:
  the shared, hash-once representation is the reason it is a value. Do not
  curry the operations: default arguments are the ergonomic win, and adaptors
  give the same pipelines without fighting arity. Do not try to make `Kernel`
  a pure value: it *is* the effect, and the design is honest about that.

**Suggested order.** (1) The small, sharp things: `Feature::Prime`, the user
directory, the parser's superlinear sum, `eval_expr`'s semantics, the frame
nonce, the buffer cap. (2) The opaque `Integer`, which halves what consumers
pay. (3) The naming decision, then `result` everywhere with the new
`Failure` — items 1, 2 and 10, which change every signature once. (4)
Adaptors, `match` and `fold` — items 3, 4 and 7, additive. (5) Assumptions as
values and `Query`/`Statement` — items 5 and 6, the redesign. (6) CI and the
property tests alongside all of it.
