# Roadmap to 1.0

Proxima is at **0.3.0**. This is the plan for getting from here to **1.0.0**:
what is left to do, in what order, and what 1.0 will promise.

It is a plan, not a contract. The order of the milestones reflects what
blocks what, and items move as work shows what matters. Each item comes from
[TODO.md](TODO.md), the [design history](docs/design.md), what building the
library has turned up, or a review of its API against what comparable
libraries — SymPy, SymEngine, GiNaC — offer.

## Where 0.1 stands

The core is built, reviewed and tested:

- **Expressions:** exact, immutable values, with normalisation, `match` and
  traversal.
- **Maxima:** the operations answered by Maxima through a kernel that caches,
  recovers from crashes and hangs, and cannot be deadlocked by a question.
- **Assumptions:** passed as values with each question.
- **Results:** `result<T>` built on FXT, with a `Cause` for each failure.
- **Local work:** rendering to text, LaTeX and MathML; numeric evaluation;
  both parsers.
- **Testing:** CI covers Linux and Windows, clang-tidy, formatting, fuzzing
  of both parsers, and the documentation site. The documentation's examples
  are compiled and run as tests.

Two review rounds are closed in [TODO.md](TODO.md), with no open items.
What 0.1 does not yet offer is the promise that makes it safe to depend on:
a stable API, releases, and support beyond the installations it was built on.

## What 1.0 will mean

1.0 is a promise, not a feature count:

- **A stable public API under semantic versioning.** Everything in
  `include/proxima/` outside `namespace detail`. No breaking change without a
  2.0; deprecations announced a minor version before removal.
- **Named platforms and Maxima versions,** each tested in CI.
- **Documentation for every public declaration,** with the examples tested.
- **Releases:** tagged, with a changelog, installable with `find_package` or
  consumable as a CMake subproject.

ABI stability is **not** promised: Proxima is a static library built with your
compiler.

---

## 0.2 — Ready to depend on

*Released as 0.2.0 on 2026-09-19.*

The library works; this milestone makes it easy and safe to *use from another
project*, and closes the one gap that breaks a workflow the documentation
presents as central.

- [x] **Evaluate what Maxima answers.** "Solve or integrate with Maxima, then
  evaluate in C++" breaks on everyday answers, because `eval_numeric` and
  `Compiled` know a fixed set of functions. Checked against Maxima 5.50:
  `integrate(tan(x), x)` is `log(sec(x))`, and `sec` cannot be evaluated;
  `∫₀^∞ xⁿe⁻ˣ dx` is `gamma(1 + n)`, and `gamma` cannot either; nor can
  `factorial`, so `5!` cannot; and `solve(x^2 = -1)` gives `-%i`, with no
  complex evaluation at all. Add `sec`, `csc`, `cot`, `gamma` (`std::tgamma`),
  `factorial` and `double_factorial`; a complex evaluation mode with
  `std::complex`; and, for a function still unknown, a way to fall back on
  Maxima's `to_float` rather than fail.
  *Done:* those six, the reciprocal hyperbolics and every inverse, `erfc`,
  and `realpart` and its kin; `eval_complex`, on Maxima's side of every
  branch cut; and `to_double`/`to_complex`, which ask Maxima only for what
  cannot be evaluated locally. A test checks every function against
  Maxima's `float`, at real and complex points.
- [x] **Builders for every function numeric evaluation knows,** and the
  reverse. `atan2`, `min`, `max` and `mod` can be evaluated but have no
  builders in `<proxima/functions.hpp>`; `sec`, `csc`, `cot`, `gamma` and
  `factorial` have neither. One list, checked by a test.
  *Done:* `numeric_functions()` is the list.

- [x] **Build the examples only as the top-level project.** `demo`, `tour` and
  `functional` are added unconditionally today, so a project that pulls
  Proxima in with `add_subdirectory`, FetchContent or CPM builds them too.
  Add a `PROXIMA_BUILD_EXAMPLES` option, defaulting to
  `PROJECT_IS_TOP_LEVEL`, like the tests.
  *Done:* the examples have their own `examples/CMakeLists.txt`, behind
  the option.
- [x] **Test both ways of consuming Proxima in CI.** A small consumer project
  built against the *installed* library (`find_package`), and one that pulls
  in the source (FetchContent or CPM). Today neither runs in CI.
  *Done:* `tests/consumer`, built both ways by the `consumers` job, with GCC
  14 on Linux and MSVC on Windows.
- [x] **Test with MinGW GCC in CI.** It is the main Windows toolchain in use,
  and the one with a quirk of its own: `std::print` needs `libstdc++exp`
  there. CI covers MSVC only.
  *Done:* the `mingw` job, with MSYS2's UCRT64 GCC, builds everything,
  examples included, and runs the Maxima-free suite.
- [x] **Run the test suite under AddressSanitizer and UndefinedBehavior
  Sanitizer.** Only the fuzzers run instrumented today.
  *Done:* the `sanitize` job runs the whole suite, Maxima included, under
  both. The `linux-sanitize` and `wsl-sanitize` presets do the same locally.
  The first run found nothing.
- [x] **A release process.** `CHANGELOG.md`, git tags, a release checklist, and
  the version kept in one place (`project()` in `CMakeLists.txt`, which the
  docs already read).
  *Done:*
  - `CHANGELOG.md` and the checklist in `RELEASING.md`;
  - a workflow that publishes a GitHub release for a pushed `vX.Y.Z` tag,
    refusing one that does not match `project()`;
  - the last copies of the version gone from the tests and the docs;
  - before 1.0, `find_package` accepts only the same minor version.
- [x] **Publish the documentation** at `docs.kinetiq.dev/proxima`: set
  `html_baseurl`, and deploy from CI or from CLion.
  *Done:* `docs/sphinx/deploy.sh`, run by CI's `deploy-docs` job for a
  release tag or a run started by hand, and by the `docs-deploy` target.
  First published 2026-09-19.
- [x] **Fix the fuzzing build's examples.** In a build configured for fuzzing,
  the examples fail to link, because the library is instrumented and the
  examples are not linked with the sanitizer. Resolved by the first item, or
  by giving them the same flags.
  *Done:* by the flags. The instrumented library passes the sanitizer
  runtime on to whatever links it, so the examples and the tests link and run
  in a fuzzing build. The CI fuzz job builds `demo` and `tour` with the
  fuzzers, to keep it so.

## 0.3 — Maxima, wherever it is

*Released as 0.3.0 on 2026-09-20.*

Proxima runs a Maxima built on SBCL, found where installers put it. This
milestone is about *which* Maxima a program gets, and how much say it has in
that. Widening the set of Maximas — other versions, other Lisps, other
platforms — is deferred below until there is a reason.

- [x] **Searching as a policy, not a default that cannot be turned off.** A
  `Config` setting — configured only, configured and the environment, or the
  full search — so that an embedder who wants no surprises gets a clear error
  instead of whatever Maxima happens to be on the machine. Alongside it,
  decide what the automatic search should still cover: `Config::sbcl_exe`,
  `Config::maxima_core`, `MAXIMA_ROOT` and a launcher on `PATH` carry most
  cases, which leaves the conventional locations. Dropping them is tempting —
  scanning `C:\`, `Program Files` and `/opt`, matching directory names and
  taking the newest is the guesswork — but on Windows nothing puts Maxima on
  `PATH`, so that scan is what makes the library work out of the box there.
  The Unix scans of `/usr`, `/usr/local` and `/opt` are already covered by a
  launcher on `PATH`.
  *Done:* `Config::search`, with `Search::Configured`, `Search::Environment`
  and `Search::Automatic` (the default). What the automatic search covers is
  unchanged: the scans stay, since on Windows nothing puts Maxima on `PATH`,
  and a program that wants none of them now says so.
- [x] **Windows paths with non-ASCII characters** work only through 8.3 short
  names, which exist only on drives with short-name generation on. Find a way
  that does not depend on them, or document the limit where users will see it.
  *Done:* the working directory is the way. It reaches the child as wide text
  through `CreateProcessW`, so a core named on its own and resolved against
  its own directory never passes through the ANSI conversion. Short names are
  still used where the volume has them, which leaves the working directory
  the caller's; the fallback applies only where it is that or nothing.
  Measured against SBCL 2.6.7 under `C:\proxima-探索-test`: the full path
  fails, the bare name with that working directory loads.

### Deferred

Worth doing, but not now. Each widens where Maxima may come from, and none
blocks 1.0; they come back when there is a reason — a user on that platform,
a distribution that ships only that build.

- **Decide and test the supported Maxima versions.** Everything is tested
  against 5.50 only, the version the golden transcripts were recorded with.
  Choose a range, test the oldest and newest in CI, and document it.
- **Support Maxima built on ECL.** Proxima's protocol is plain Common Lisp —
  the helper it loads has nothing SBCL-specific — so this should be a second
  launcher, beside SBCL's: how to find an ECL Maxima and start it. It would
  also make Maxima from Ubuntu usable, if its `maxima-sage` package is the
  ECL build it appears to be (to check). Today Ubuntu has no Maxima Proxima
  can use at all.
- **macOS.** Discovery for Homebrew's and MacPorts' Maxima, and a CI job.
  Untested today.

### Decided against

- **Assembling and shipping a copy of Maxima.** Proxima is a library, not an
  application: which Maxima to ship, and how to trim it, belongs to the
  program that ships one. What the library owes such a program is a way to
  say where its copy is, and `Config::sbcl_exe` and `Config::maxima_core` are
  that — they assume no layout at all. The
  [how-to](docs/sphinx/how-to/choose-maxima.md) shows the use; the library
  will not go looking beside the executable or build the copy itself.

## 0.4 — The shape of 1.0

Decisions that change the public API, made before it is frozen.

- [x] **Review the public API as a whole.** Names, parameter order, which
  header declares what, and what is public but should be in `detail` (the
  display tree in `render.hpp` is public only because a template needs it).
  *Done* against 0.3.0, every public header read as one surface. Two
  documentation slips were fixed on the spot; everything else is a decision,
  and is listed under *From the API review* below.
- [ ] **`Expr::str()` and `Expr::parse`: members or free functions?** They are
  members of `Expr` but implemented in `src/render` and `src/parse`, which
  keeps the expression model from being built as a library of its own — one
  that needs no Boost.Process, for users who never call Maxima. Decide
  whether that split is wanted; it is an API change either way
  ([TODO §9.5](TODO.md)). Two things the review adds. The names: `Expr::str()`
  beside `Integer::to_string()` beside the free `to_tex` and `to_mathml` are
  three spellings of "as text"; whichever way the members go, pick one. And a
  free `parse` already exists — `proxima::parse`, Maxima's own parser, which
  needs a kernel — so a free local one cannot take that name without a
  qualifier, and the pair should say which is which.
- [ ] **How renderers extend.** Adding a construct to the rendering walk (as
  `Postfix` was added in 0.1) adds enumerators to `Construct` and `Slot`,
  which breaks a user's exhaustive `switch`. Decide the rule — enumerators
  may be added in minor versions, or the enums are closed — and document it.
- [ ] **Deprecated names.** `simplify` is an older name for `ratsimp`. Remove it
  before 1.0, or keep it for good.
- [ ] **Relations written infix.** `==` cannot be overloaded to build an
  equation, since it is structural equality returning bool, so relations are
  `eq(x * x, 4)`. The named-operator trick would allow
  `x * x <rel::eq> 4`: a tag object per relation, `operator<` returning the
  left side and `operator>` completing it, each a hidden friend of the type
  it takes. Tried as a prototype: about 30 lines, clean on GCC 16, Clang and
  MSVC `/W4`, with arithmetic binding tighter as wanted and a plain number
  allowed on either side. What sinks it as written is clang-format, which
  spaces it to `x * x < rel::eq > 4` — the same expression, but no longer
  looking like an operator, and formatting is enforced here. Decide whether
  an opt-in `<proxima/relops.hpp>` is worth it anyway, or whether `eq` stays
  the only spelling.
- [ ] **Use the C++23 library where it is now available.** `std::ranges::to`
  was waiting for GCC 14 to be the oldest compiler, which it is now
  ([TODO §9.4](TODO.md)). `std::generator` still waits for libc++;
  `std::flat_map` for GCC 15.
- [ ] **Document every public declaration.** Turn on Doxygen's warnings for
  undocumented members in the docs build, so the reference cannot fall
  behind.

### From the API review

What reading every public header as one surface turned up. Each is a
decision rather than a bug, and most are renames, which is why they are here
and not later: after 1.0 they cannot be made.

- [ ] **One vocabulary for the node kinds.** The same node has two names
  wherever it appears: `Kind::Add`, `Mul`, `Pow` and `Function` against the
  views `node::Sum`, `Product`, `Power` and `Call`, and the renderer's
  `Construct::Sum`, `Product`, `Power`, `Call`. Pick one — the views' and the
  renderer's, most likely — and rename the enumerators.
- [ ] **One convention for an enum's spelling.** Four public enums are spelled
  four ways: `to_string(Cause)`, `name_of(Feature)`, `kind_name(Kind)` and
  `symbol_for(RelOp)`; `Truth`, `Side` and `Search` have none; and only `Kind`
  has a `std::formatter`. Pick one name, and give every public enum a
  formatter.
- [ ] **The names of the ways to a number.** `eval_numeric` and `eval_complex`
  evaluate locally; `to_double` and `to_complex` evaluate locally and fall
  back on Maxima; `to_float` is Maxima's `float`, and returns an `Expr`. Two
  prefixes with a meaning, and one name that fits neither. Decide the grid,
  and give `to_float` a name that says it keeps the expression.
- [ ] **Optional arguments ahead of the `Env`.** `diff(f, x, 1, kernel)`,
  `limit(f, x, 0, Side::Both, kernel)`, `coeff(e, t, 1, env)`: a kernel cannot
  be passed without first spelling out a default the caller never wanted to
  mention, and the examples now show that at every call. Overloads without
  the optional parameter, or the `Env` ahead of it — decide, and apply the
  rule to every operation.
- [ ] **A shared kernel that can be configured.** `shared_kernel()` takes no
  `Config`, so a program that wants the default kernel — the one every
  operation uses when no `Env` is given — to run a particular Maxima, or to
  keep a disk cache, has no way to say so, and must thread a `Kernel` of its
  own through every call. That is exactly what the examples had to do. A way
  to configure it before first use, or a documented decision not to.
- [ ] **Solving a system takes spans.** `solve(equations, unknowns)` takes
  `std::span<const Expr>` and `std::span<const Symbol>`, so
  `solve({eq(x + y, 3), eq(x - y, 1)}, {x, y})` does not compile and every
  caller builds two vectors first. `initializer_list` overloads, or ranges.
- [ ] **The exceptions and the causes do not line up.** `unwrap` throws
  `ParseError`, `EvalError` or `OverflowError` for those three causes and
  `MaximaError` for every other, so `unwrap(solve(eqs, {}))` throws
  `MaximaError` — `Cause::Argument` — although Maxima was never asked. The
  builders throw the base `Error` for a zero denominator or a NaN, which no
  cause names. Decide the mapping: one exception per cause, or one exception
  carrying the cause.
- [ ] **Reading text: `optional`, `result` or throw.** `Integer::parse` returns
  an `optional`, `Expr::parse` a `result`, and `Integer(std::string_view)`
  throws `ParseError`. One convention.
- [ ] **What is in `detail` but belongs outside.** `detail::with_operands` —
  a node rebuilt with new operands, normalised — is the one primitive a
  user's own walk needs, and `traverse.hpp` says as much; promote it.
  `detail::same_representation` is the identity test `transform` relies on,
  and a user's rewrite wants it for the same reason. Go through the rest of
  `detail` in the public headers with the same question.
- [ ] **`Symbol` is declared in one header and defined in another.**
  `expr.hpp` forward-declares it, constrains `ExprArgument` and `pow` by it
  and returns it from `Expr::as_symbol` — with a note to include
  `symbol.hpp` first. A class of one member that the core header already
  depends on belongs in the core header; merge `symbol.hpp` into it, or
  move `as_symbol` out.
- [ ] **`result` is lower-case among upper-case types.** `result<T>` is
  spelled as FXT spells `fxt::result`, while `Failure` is upper-case although
  it is `fxt::failure`. Decide the spelling of both, and whether `result` is
  an alias of FXT's or a name of Proxima's own.
- [ ] **`as_function` is `Compiled` under another name.** `as_function(e, x)`
  returns the `Compiled` that `Compiled(e, x)` constructs, and `compile(e, x)`
  is the same as a `result`. Three entry points to one preparation; keep the
  two that differ in how they fail, and decide whether the third earns its
  name.
- [ ] **`[[nodiscard]]` and `noexcept`, audited once.** Only the `with()`
  methods are `[[nodiscard]]`; `str()`, `to_tex`, `eval_numeric`,
  `canonical_order` and the rest of the pure functions are not. Accessors
  that cannot throw — `kind()`, `hash()`, `is()`, `arity()` — are not
  `noexcept`. Go through every declaration once, before 1.0 makes the
  contracts permanent.
- [ ] **Small things, in one pass.** `Integer` has `+=`, `-=`, `*=` and `/=`
  but no `%=`. `nroots`'s defaults are `Expr::symbol("minf")` and
  `Expr::symbol("inf")` where `minf()` and `inf()` exist. Bounds are
  `from`/`to` in `integrate` and `low`/`high` in `nroots` and `find_root`.
  `Feature`'s last enumerator carries a comment about the tests, which is
  no concern of a public header. `Kernel::tell` returns `result<fxt::unit>`,
  the one place an FXT type other than the result appears in a signature.

### From the design review

The implementation read against the SOLID principles and for states the
types allow but the code forbids — the second being the sharper question,
since every such state is a rule some function has to remember. Most of
these are internal, so they need no release to change; they are here because
the public ones among them are, and because the internal ones are what a
contributor should know before touching the session or the node.

What already does this well, and should stay as it is: `Payload`, which is
the only way to make text Maxima's reader sees, and escapes it in its two
factories; `Query` against `Statement`, so a question and an instruction are
different types and `ask` cannot take the wrong one; `Expr`, normalised at
construction so no unnormalised expression exists; `Integer`, canonical
between its inline and its wide form; `Assumptions`, sorted and deduplicated
so equal facts are one value; `Symbol` where a variable is meant; NaN refused
at the door; and the renderer concepts, four of them so the diagnostic names
what is missing, with no base class for a user's type to inherit.

**States the types allow and the code forbids**

- [x] **`Node`'s kind and payload can disagree.** `kind` is one field and
  `payload` a variant beside it, so a node of `Kind::Integer` holding a
  string is constructible, and every one of the eight builders sets both by
  hand; the accessors then `std::get` on trust. And `Add`, `Mul`, `Pow` and
  `Relation` all hold a `std::vector<Expr>`, so a power with three operands
  or a relation with one is representable, and `match` indexes `[0]` and
  `[1]` unchecked. `rel_op` is meaningful for a relation only and present
  everywhere, defaulting to `Equal`. Let the variant *be* the kind — one
  alternative per kind, a `Relation{op, lhs, rhs}` among them, and a pair
  rather than a vector where there are exactly two — and derive `kind()`
  from its index. Internal; the library's central invariant should not rest
  on discipline in eight places.
  *Done:* one alternative per kind, `kind()` read off the index, the
  two-operand kinds built only through a constructor. `Expr::args()` still
  hands out a vector, so those keep vector storage behind it; a span is the
  public change, listed with the API findings' small things.
- [x] **`Reply` and the cause, by string prefix.** `Reply` is `{ok, value,
  reason}`, so a success with a reason and a failure with a value are both
  representable; and `to_result` decides the cause by whether the reason
  *begins with* "this computation needs an assumption" — a literal that
  lives in the Lisp helper in `session.cpp`, in `kInconsistent` in
  `session.hpp`, and again in `reply.hpp`, which says "must stay in step".
  A Maxima error that happened to begin with those words would be
  misclassified. Have the helper's frame carry the kind of failure as its
  status token — `T`, `NIL`, or a third value for a question — and make
  `Reply` a value or a failure-with-cause, which the caches can serialise
  as easily as a flag.
  *Done.* The helper now prints `Q` for a question it intercepted, `NIL`
  for a Maxima error and `T` for a value, so the cause is decided where the
  failure happens. `Reply` is a `variant<string, Failed>` behind
  `ok()`/`value()`/`reason()`/`cause()`; the three literals and
  `kInconsistent` are gone. The persistent format is `proxima-cache-2`: one
  cause field and one text field, where there were a flag and two texts.
- [x] **`MaximaSession`'s lifecycle is booleans and sentinels.** `recovering_`,
  `state_accounted_`, a `factory_` that may be empty (three constructors,
  one of which makes "a death final"), a `transport_` that may be null, and
  `active_context_` using the empty string for "unknown". Together they
  encode a small state machine — running, recovering, cannot restart —
  that no type spells out, and `restart()` finds the last of those by
  testing the factory at the moment it is called. An explicit state, and
  `std::optional` where the empty string stands for "not known".
  *Done, in part.* The state machine is now `enum class Phase { Running,
  Recovering, CannotRestart }`, and the transport-only constructor holds a
  factory that *says* it cannot restart rather than no factory at all, so
  nothing tests emptiness. `transport_` is reached through an accessor that
  reports rather than dereferencing null. Two of the review's points needed
  no change: `active_context_` was already a `std::optional`, and
  `state_accounted_` is an honest flag — one fact, two values — not a state
  machine in disguise.
- [x] **`SExpr` and `DisplayNode` carry every field for every kind.** Both
  are the shape `Node` had before it became a variant: a kind beside
  `text`, `real`, `items` and the rest, with `digits()` "meaningless for
  other kinds — check first". `DisplayNode` also keeps `negated` as a
  vector parallel to `children`, which the walk guards with a length check;
  a `std::vector<Term>` of child-and-sign pairs cannot be mismatched. The
  same treatment as `Node`. `DisplayNode` is in a public header, so it
  belongs with the renderer-extension decision above.
  *Done.* Both are variants now, one alternative per kind, with `kind()`
  read off the index. `SExpr` has a type per string kind, so `digits()` on
  a symbol throws where it used to return the symbol's name.
  `DisplayNode` has fourteen alternatives; the fixed-arity ones — fraction,
  power, root, relation, negation, postfix — take their operands through a
  constructor, and a sum is a `std::vector<DisplayTerm>` of addend-and-sign,
  so the two can no longer be of different lengths and the walk's
  `i < negated.size()` guard is gone. The walk is a `std::visit`, so a kind
  reaches only for what it has and there is no fallthrough rendering an
  empty verbatim for a kind nobody handled.
  The renderer-extension decision is untouched: `DisplayNode` is in
  `proxima::detail`, nothing outside `src/render` builds one, and what a
  user's renderer sees — the concepts, `Term<T>`, `Construct`, `Slot` — is
  unchanged.
- [ ] **`Config` allows one of `sbcl_exe` and `maxima_core` without the
  other,** and discovery refuses it at runtime. A single optional value
  holding both — `std::optional<NamedInstallation>` — makes half a name
  unrepresentable. Public, so a 0.4 change.
- [ ] **A `Solution` is positional.** `Solution` is `std::vector<Expr>`, with
  `solution[i]` belonging to `unknowns[i]` by convention only; nothing ties
  a solution to the unknowns it answers. A value that carries its symbols
  — a small map, or a struct with `value_of(x)` — cannot be read against
  the wrong list. Public.
- [ ] **An assumption need not be a relation.** `assuming(Expr(5))` compiles,
  and fails inside Maxima on first use, with the context machinery in
  between. A `with(Expr fact)` that requires a relation, or a `Fact` type
  the relation builders return, would refuse it where it is written.
  Public.
- [ ] **The head tables run in both directions by hand.** `kDisplayNames` in
  `from_maxima.cpp` and `kInternalHeads` in `to_maxima.cpp` are the same
  four pairs written twice, one the reverse of the other, plus `list`; an
  entry added to one and not the other is a head that arrives but does not
  go back. One table, read both ways.
- [x] **A default-constructed `Nodes::iterator` was not at the end.** It
  dereferenced to the integer zero and compared unequal to the sentinel.
  *Fixed* in this review: a default iterator is done.

**SOLID, where it is worth a change**

- [x] **`MaximaSession` has five jobs.** Framing the protocol (`request_for`,
  `read_frame`, the delimiters), keeping the transport alive (`recover`,
  `handshake`), deciding what is cached and when (`eval_pure` against two
  caches and a generation counter), managing Maxima's contexts for
  assumptions (an LRU of sixteen, `select_environment`, `switch_context`),
  and the launch recipe (`launch_command`, `launch_environment`, the
  private user directory). Eight hundred lines, two locks, and a public
  interface of static helpers "exposed for testing" because the pieces
  cannot be tested apart. Three of them come out cleanly: a `Framing`
  unit, a `ContextTable`, and a `Launcher`; each is already internally
  coherent and mostly static.
  *Done:* `kernel/protocol` (Payload, the delimiters, the request
  wrapper, the helper Lisp, and reading a frame), `kernel/launch` (the
  command line, the environment, the private user directory, and starting
  Maxima for a Config) and `kernel/context_table` (the LRU of contexts,
  tested on its own). The session keeps the conversation: 400 lines, from
  800, and no static helpers exposed for testing.
- [ ] **Adding a construct to rendering touches six places.** `DisplayKind`,
  `Construct`, `Slot`, `to_display`, `render_node` and `construct_of` all
  change together — seven now, with the alternative beside `DisplayKind`,
  though a missing arm is at least a compile error — and a user cannot add
  one at all. That is the closed
  side of the renderer design, whose open side — a new renderer needs no
  change here — is the part that works. Already the "How renderers extend"
  decision above; this is the shape of the cost.
- [ ] **`unwrap`'s mapping from cause to exception is a switch to keep in
  step with `Cause`,** with `default` swallowing every cause it does not
  name into `MaximaError`. Listed among the API findings; the design
  point is that an enum and a switch on it in another header are two
  things to change for one addition.
- [ ] **A clock the session cannot be given.** `read_frame` reads
  `steady_clock` directly, so every timeout test waits out real time in
  50 ms polls. Not urgent — the tests pass — but the one dependency the
  session does not take from outside, where it takes the transport, the
  environment and the command runner.

Kept as they are, deliberately: `Kernel` as a thin facade over the session;
`Config` as one flat struct, since grouping its fields would be a public
change for little; the `if` chain in `from_maxima`, which maps a closed set
of Maxima heads and reads better than a table would; and the two evaluators
sharing one builtin table in `numeric.cpp`, which is the data-driven shape
this library should have more of.

### Local tools users of SymPy or GiNaC will look for

All pure, needing no kernel.

- [ ] **The symbols in an expression.** Only `contains(e, x)` exists; listing
  the free symbols is what lets an expression be compiled without naming its
  variables by hand.
- [ ] **Substituting several symbols at once.** `replace` and `subst` take one
  symbol each, so a simultaneous substitution — swapping `x` and `y` — cannot
  be written correctly.
- [ ] **Numerator and denominator** of an expression, not only of a rational
  number.
- [ ] **Logical connectives:** `and`, `or` and `not`, for predicates asked with
  `is` and for assumptions that are not a plain conjunction.

### The kernel's API

- [ ] **Load Maxima packages without losing the disk cache.** A package needs
  `tell(Statement::text("load(...)"))`, and a `tell` stops persistence for the
  kernel. A `Config` list of packages loaded at startup would be known before
  any question, so it can go into the cache key and persistence can stay on.
- [ ] **A timeout per call, and cancellation.** Only the kernel-wide
  `set_timeout` exists. A GUI's cancel button needs a way, callable from
  another thread, to stop the current question — which for Maxima means ending
  the process and starting another, as a timeout already does.
- [ ] **Asynchronous questions.** Operations returning a `std::future`, or a pool
  of kernels, instead of each user writing the threads the
  [threads how-to](docs/sphinx/how-to/use-threads.md) shows.
- [ ] **A way to watch the traffic.** A hook, off by default, that sees each
  question and answer as they pass to and from Maxima, for when an answer
  looks wrong.
- [ ] **Capture what Maxima prints.** A question answers with its value, and
  anything Maxima prints on the way is discarded: `(print("hello"), 42)` is
  just `42`. So whatever works by printing gives nothing back — `describe`,
  `display`, Maxima's own `tex()`, and the code generators `f90`, `gentran`
  and `pytranslate`. Offer the printed text beside the value, for the
  questions that ask for it.

### Instructions given as text

`kernel.ask(Query::text(...))` is how Maxima carries out an instruction
written as text. Three things about it are easy to get wrong:

- [ ] **Only the first statement counts.** `"a: 2$ a + 1"` answers `2`: Maxima's
  `eval_string` stops after one statement, and the rest is dropped without a
  word. Refuse text with more than one statement, or evaluate them all and
  answer with the last.
- [ ] **A query that changes Maxima goes unnoticed.** That same query really
  does assign `a`, but a query is taken to be a question, whose answer is
  cached and kept on disk. Recognise text that assigns, defines or loads, and
  refuse it or treat it as a `tell` — or at least say so in the failure.
- [ ] **Syntax errors say nothing useful.** Text Maxima cannot read fails with
  "An error was caught by errcatch" and no position. Pass on what Maxima's
  parser said.

## 0.5 — Coverage

The operations a user of a computer algebra system will look for. A proposal,
to be shaped by what users ask for; each addition is a wrapper, its failure
causes, tests against Maxima, and a page in the docs.

- [ ] **Initial conditions for ODEs:** `ic1` and `ic2`, beside `ode2`.
- [ ] **Transforms:** Laplace (`laplace`, `ilt`).
- [ ] **Linear algebra:** matrices as a typed expression, with determinant,
  inverse, eigenvalues and solving linear systems in matrix form.
- [ ] **Polynomials:** `gcd`, division with remainder, resultant,
  `ratsimp`-free coefficient access.
- [ ] **Numeric integration** through Maxima's QUADPACK (`quad_qags` and
  friends), for integrals with no closed form.
- [ ] **Complex numbers:** real and imaginary parts, conjugate, and rectangular
  and polar forms.
- [ ] **Inequalities:** solving them, which `solve` does not.
- [ ] **More differential equations:** systems of ODEs, and numeric solutions.
- [ ] **Series that keep their order.** `taylor` returns an ordinary expression,
  so the order it was truncated at is lost.
- [ ] **Arbitrary precision.** Numbers stop at `double`. A Maxima bigfloat at
  the default precision comes back as the exact rational it equals, but as
  Maxima text in an `Opaque` rather than a `Rational`; one of high precision
  (`fpprec: 30`, say) comes back as an untyped `bigfloat(mantissa, exponent)`
  call that does not read back as a bigfloat. Offer bigfloats as a value that
  keeps its precision, and evaluates.
- [ ] **Typed values for Maxima's other kinds of answer.** Everything comes back
  as an `Expr`, but only numbers, symbols, sums, products, powers and
  relations have typed nodes. The rest arrive generically: a matrix is a call
  to `matrix` holding lists of rows, a set a call to `set`, a string raw text
  in an `Opaque`, a truth value the symbol `true` or `false`. Give sets,
  strings and truth values types of their own, as the matrix item above does
  for matrices, so that a caller can ask for one rather than take one apart.

Anything not wrapped stays reachable through `Kernel::ask` — see the
[how-to](docs/sphinx/how-to/call-unwrapped-maxima.md) — so this list is about
convenience and typed results, not about what is possible. Checked across the
manual's chapters against Maxima 5.50: flags, polynomials, special and
elliptic functions, `algsys`, QUADPACK, matrices, series, number theory, sets,
Laplace transforms, program flow, user-defined functions, and the packages
`distrib`, `fourier_elim`, `to_poly_solve` and `stringproc` all answer through
`ask`, with packages loaded by `tell`. Out of reach are only plotting, which
needs a window or a file, the interactive chapters (help, debugging, the
command line), and output Maxima prints rather than returns — the item in
0.4.

And two kinds of output:

- [ ] **A Unicode pretty-printer in the library,** drawing fractions, powers and
  roots in two dimensions. One exists as the example
  [`examples/text2d.hpp`](examples/text2d.hpp), not as part of Proxima.
- [ ] **Code generation:** an expression as C++ source, to compile into another
  program, where `Compiled` evaluates at run time.

## 1.0.0

- [ ] The API reviewed in 0.4 frozen, and the stability promise above written
  into the README and the docs.
- [ ] Every platform and Maxima version named in the docs passing in CI.
- [ ] Documentation complete: every public declaration documented, the user
  guide and how-tos current, all examples tested.
- [ ] Performance measured on reference machines and written down — the
  timings in [TODO.md](TODO.md) are one machine's.
- [ ] A longer fuzzing run, nightly rather than a minute per push.
- [ ] Release: changelog, tag, published documentation.

---

## After 1.0

Not needed for 1.0, and worth doing once the base is stable:

- **Proxima in the browser.** Maxima already runs in WebAssembly, built on
  ECL. With ECL support (0.3) and Proxima compiled with Emscripten, a new
  transport could talk to it in a Web Worker instead of over a pipe.
  Licensing needs looking at: Maxima is GPL, and a module inside a web page is
  not the separate program it is today.
- **Package managers:** vcpkg and Conan ports. FXT would need one too.
- **`std::generator`** for traversal, once libc++ has it.
- **A local-only library** — expressions, parsing, rendering and numbers,
  without Boost.Process — if 0.4 decides for the split.
