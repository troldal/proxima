# Changelog

What changed in each release of Proxima, newest first. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the versions
[semantic versioning](https://semver.org): before 1.0, a minor release may
change the API. [RELEASING.md](RELEASING.md) says how a release is made, and
[ROADMAP.md](ROADMAP.md) what the next ones hold.

## [Unreleased]

Toward 0.2.0, *ready to depend on*.

### Added

- **Numeric evaluation of more functions.** `eval_numeric` and `Compiled` now
  evaluate:
  - `sec`, `csc`, `cot`, `sech`, `csch` and `coth`, and every inverse;
  - `gamma`, `factorial`, `double_factorial` and `erfc`;
  - `realpart`, `imagpart`, `conjugate`, `cabs` and `carg`.

  So `log(sec(x))`, Maxima's integral of `tan(x)`, now evaluates, and so do
  `gamma(1 + n)` and `5!`.
- **`eval_complex`.** It evaluates over the complex numbers, with `%i` the
  imaginary unit. On a branch cut, it takes the side Maxima takes.
- **`to_double` and `to_complex`** in `<proxima/ops.hpp>`. They evaluate
  locally when they can, and otherwise ask Maxima to apply `float`.
- **`numeric_functions()`**, which lists the functions numeric evaluation
  knows.
- **Builders** in `<proxima/functions.hpp>` for every function numeric
  evaluation knows, `atan2`, `mod`, `max`, `min` and `round` among them.
- **`PROXIMA_BUILD_EXAMPLES`**, which is on only when Proxima is the top-level
  project.
- **`linux-sanitize` and `wsl-sanitize` presets**, which build and test under
  AddressSanitizer and UndefinedBehaviorSanitizer.
- **More CI:**
  - a consumer project built against the installed package and with
    FetchContent;
  - MinGW GCC;
  - the whole suite under the sanitizers;
  - a release workflow.

### Changed

- **Package version compatibility.** Before 1.0, `find_package(proxima 0.2)`
  accepts only 0.2.x. It used to accept any 0.x.
- **Clang 20 or later is required.** Clang 19 cannot compile FXT's adaptors
  against GCC's standard library.

### Fixed

- **Pulling Proxima in as a subproject** (`add_subdirectory`, FetchContent or
  CPM) no longer builds its examples.
- **In a fuzzing build,** whatever links the instrumented library (the tests
  and the examples) now links its sanitizer runtime too.

## [0.1.0] — 2026-09-19

The first version: `master` as of `2cbd7ef`, before work toward 0.2.0 began.
Tagged `v0.1.0` afterwards.

### Added

- **Expressions as immutable values:**
  - hashed once and compared structurally;
  - normalised as they are built;
  - exact, unbounded arithmetic;
  - read with `Expr::match`.
- **Operations answered by Maxima,** each returning a `proxima::result`:
  `diff`, `integrate`, `limit`, `solve`, `ode2`, `sum`, `product`, `taylor`,
  `factor`, `expand`, `ratsimp`, `trigsimp`, `radcan`, `is`, `find_root` and
  more.
- **Assumptions as values,** passed with the question.
- **The kernel:**
  - a Maxima child process over pipes;
  - thread-safe;
  - restarted if Maxima dies or hangs;
  - caching answers in memory and, optionally, on disk.
- **Two parsers:** `Expr::parse`, offline, and `proxima::parse`, Maxima's
  own.
- **Rendering:** infix text, LaTeX and MathML, and user-defined renderers.
- **Numeric evaluation with no kernel:** `eval_numeric` and `Compiled`.
- **Results as FXT's `expected`,** for pipelines with FXT's adaptors.
- **CMake packaging,** and a documentation site built with Sphinx, Doxygen and
  Breathe.

[Unreleased]: https://github.com/troldal/proxima/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/troldal/proxima/releases/tag/v0.1.0
