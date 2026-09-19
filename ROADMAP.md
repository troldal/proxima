# Roadmap to 1.0

Proxima is at **0.1.0**. This is the plan for getting from here to **1.0.0**:
what is left to do, in what order, and what 1.0 will promise.

It is a plan, not a contract. The order of the milestones reflects what
blocks what, and items move as work shows what matters. Each item links to
where it comes from — [TODO.md](TODO.md), the [design history](docs/design.md),
or what building the library has turned up.

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

The library works; this milestone makes it easy and safe to *use from another
project*.

- [ ] **Build the examples only as the top-level project.** `demo`, `tour` and
  `functional` are added unconditionally today, so a project that pulls
  Proxima in with `add_subdirectory`, FetchContent or CPM builds them too.
  Add a `PROXIMA_BUILD_EXAMPLES` option, defaulting to
  `PROJECT_IS_TOP_LEVEL`, like the tests.
- [ ] **Test both ways of consuming Proxima in CI.** A small consumer project
  built against the *installed* library (`find_package`), and one that pulls
  in the source (FetchContent or CPM). Today neither runs in CI.
- [ ] **Test with MinGW GCC in CI.** It is the main Windows toolchain in use,
  and the one with a quirk of its own: `std::print` needs `libstdc++exp`
  there. CI covers MSVC only.
- [ ] **Run the test suite under AddressSanitizer and UndefinedBehavior
  Sanitizer.** Only the fuzzers run instrumented today.
- [ ] **A release process.** `CHANGELOG.md`, git tags, a release checklist, and
  the version kept in one place (`project()` in `CMakeLists.txt`, which the
  docs already read).
- [ ] **Publish the documentation** at `docs.kinetiq.dev/proxima`: set
  `html_baseurl`, and deploy from CI or from CLion.
- [ ] **Fix the fuzzing build's examples.** In a build configured for fuzzing,
  the examples fail to link, because the library is instrumented and the
  examples are not linked with the sanitizer. Resolved by the first item, or
  by giving them the same flags.

## 0.3 — Maxima, wherever it is

Proxima runs a Maxima built on SBCL, found where installers put it. This
milestone widens that.

- [ ] **Decide and test the supported Maxima versions.** Everything is tested
  against 5.50 only, the version the golden transcripts were recorded with.
  Choose a range, test the oldest and newest in CI, and document it.
- [ ] **Support Maxima built on ECL.** Proxima's protocol is plain Common Lisp —
  the helper it loads has nothing SBCL-specific — so this should be a second
  launcher, beside SBCL's: how to find an ECL Maxima and start it. It would
  also make Maxima from Ubuntu usable, if its `maxima-sage` package is the
  ECL build it appears to be (to check). Today Ubuntu has no Maxima Proxima
  can use at all.
- [ ] **macOS.** Discovery for Homebrew's and MacPorts' Maxima, and a CI job.
  Untested today.
- [ ] **Ship Maxima with an application.** Look for a `maxima/` directory next
  to the executable before anywhere else, and add a script that assembles the
  smallest working copy from an installation. Settle what that copy needs —
  whether `share/maxima/<version>/src` is required is not yet checked. See the
  [how-to](docs/sphinx/how-to/choose-maxima.md).
- [ ] **Windows paths with non-ASCII characters** work only through 8.3 short
  names, which exist only on drives with short-name generation on. Find a way
  that does not depend on them, or document the limit where users will see it.

## 0.4 — The shape of 1.0

Decisions that change the public API, made before it is frozen.

- [ ] **Review the public API as a whole.** Names, parameter order, which
  header declares what, and what is public but should be in `detail` (the
  display tree in `render.hpp` is public only because a template needs it).
- [ ] **`Expr::str()` and `Expr::parse`: members or free functions?** They are
  members of `Expr` but implemented in `src/render` and `src/parse`, which
  keeps the expression model from being built as a library of its own — one
  that needs no Boost.Process, for users who never call Maxima. Decide
  whether that split is wanted; it is an API change either way
  ([TODO §9.5](TODO.md)).
- [ ] **How renderers extend.** Adding a construct to the rendering walk (as
  `Postfix` was added in 0.1) adds enumerators to `Construct` and `Slot`,
  which breaks a user's exhaustive `switch`. Decide the rule — enumerators
  may be added in minor versions, or the enums are closed — and document it.
- [ ] **Deprecated names.** `simplify` is an older name for `ratsimp`. Remove it
  before 1.0, or keep it for good.
- [ ] **Use the C++23 library where it is now available.** `std::ranges::to`
  was waiting for GCC 14 to be the oldest compiler, which it is now
  ([TODO §9.4](TODO.md)). `std::generator` still waits for libc++;
  `std::flat_map` for GCC 15.
- [ ] **Document every public declaration.** Turn on Doxygen's warnings for
  undocumented members in the docs build, so the reference cannot fall
  behind.

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

Anything not wrapped stays reachable through `Kernel::ask` — see the
[how-to](docs/sphinx/how-to/call-unwrapped-maxima.md) — so this list is about
convenience and typed results, not about what is possible.

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
