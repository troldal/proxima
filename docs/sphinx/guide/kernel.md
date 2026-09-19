# The kernel

A `proxima::Kernel` is one Maxima process and its caches. Every operation asks
one; `shared_kernel()` is the process-wide one, and every operation's `Env`
defaults to it.

- **Started on first use**, or constructed explicitly: `proxima::Kernel
  kernel(config);`.
- **Serialised**, so a `Kernel` is safe to share between threads. For real
  parallelism, give each thread its own — Maxima is one process doing one
  thing. See [the how-to](../how-to/use-threads.md).
- **Survives its own death.** If Maxima hangs or exits, the failing call
  reports it and the kernel is restarted, so the next call starts from a
  working session. Nothing needs replaying: assumptions come with each
  question, and the Maxima context for a set of them is made again when next
  asked for.
- **Speaks structure, not text.** An expression reaches Maxima as its internal
  s-expression and comes back the same way; the infix printer is for people
  and is not on the path. So a symbol called `x y`, or an `Opaque` holding a
  `$`, is simply a symbol or simply a question — nothing this library sends
  can be misread by Maxima's parser, and nothing Maxima *cannot* read (a
  malformed string given as a `Query`) costs more than one round trip and a
  message.
- **Two verbs, and the type says which.** `kernel.ask(Query::text("gcd(12,
  18)"))` is 6, as an `Expr`: a question, for a Maxima function this library
  does not wrap, cached like any operation and asked under assumptions like
  any operation. `kernel.tell(Statement::text("a: 7"))` carries out a
  statement: it empties the cache and stops persistence, since nothing in it
  says what it changed. Both take an `Expr` (`Query::form`,
  `Statement::form`) as well as text. There is no "evaluate, and promise it
  changes nothing" to get wrong.
- **Remembers answers.** An LRU keyed on the question *and* the assumptions it
  was asked under, so asking under different assumptions never needs to
  discard anything; only a `tell` does. Bounded by `Config::cache_entries` and
  `Config::cache_bytes` (64 MB); zero entries disables it.
- **Optionally between runs.** Set `Config::cache_directory` and answers
  survive process exit and are shared with other processes using the same
  directory. Every key carries the Maxima version, this library's version
  *and* the assumptions, so an entry can only be read back under the
  conditions that produced it — `sqrt(x^2)` asked under `x > 0` is not visible
  to a question asked under nothing. A `tell` may change Maxima in a way no
  key can describe, so it stops persistence for that kernel;
  `Kernel::persistence_active()` says so, and `Kernel::restart()` resumes it
  with a fresh Maxima. See [the how-to](../how-to/cache-between-runs.md).
- **Cannot be deadlocked by a prompt.** Maxima asks the user for facts it
  lacks, and reads the answer from standard input; over a pipe that would
  block and then swallow the next request. Questions become errors instead —
  see [Assumptions](assumptions.md#when-maxima-needs-a-fact-it-has-not-been-told).

## Configuration

`Config` covers `maxima_root`, `timeout`, `startup_timeout`, `cache_entries`,
`cache_bytes`, `cache_directory`, `cache_directory_limit` (256 MB by default,
evicting the least recently used answers), `load_user_init` and `user_dir`.
The user's own `maxima-init.mac` is **not** loaded by default: a library
should compute the same answer on every machine.

Which Maxima is used: `Config::maxima_root`, then `$MAXIMA_ROOT`,
`$MAXIMA_PREFIX`, the parent of any `$PATH` entry named `bin`, then the
conventional install locations. A root given explicitly is authoritative: if
it is wrong, that is an error rather than a reason to run some other
installation. See [the how-to](../how-to/choose-maxima.md).
