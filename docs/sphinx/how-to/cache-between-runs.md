# Keep answers between runs

**Task:** stop paying for the same integrals every time your program starts.

A kernel remembers answers in memory already. To keep them on disk, give it a
cache directory:

```cpp
#include <proxima/kernel.hpp>
#include <proxima/ops.hpp>

namespace px = proxima;

px::Config config;
config.cache_directory = "proxima-cache";       // created if missing
px::Kernel kernel(config);

const px::Symbol x("x");
px::integrate(pow(x, 2) * px::sin(x), x, kernel);   // asked once, then read from disk
```

The next run — or another process using the same directory — reads the
answer instead of asking Maxima.

## What makes it safe

Every stored answer is keyed on the question, the assumptions it was asked
under, the Maxima version and Proxima's version. An answer is only ever read
back under the conditions that produced it: `sqrt(x^2)` asked under `x > 0`
is not seen by the same question asked under nothing, and upgrading Maxima
starts afresh.

## Its size

`config.cache_directory_limit` bounds the directory, 256 MB by default; the
least recently used answers go first. The in-memory cache has its own bounds,
`cache_entries` and `cache_bytes`.

## When it stops

`kernel.tell(...)` changes Maxima in ways no key can describe, so it stops
persistence for that kernel: `kernel.persistence_active()` turns false, and
answers are neither read from disk nor written to it until
`kernel.restart()` starts a fresh Maxima.
