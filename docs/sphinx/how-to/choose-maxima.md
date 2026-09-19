# Choose, or ship, the Maxima installation

**Task:** control which Maxima your program runs, or bring one with it.

## How Proxima finds Maxima

When a kernel first starts it looks, in order, at:

1. `Config::maxima_root`, if you set it;
2. the environment variables `MAXIMA_ROOT` and then `MAXIMA_PREFIX`;
3. the parent of any directory on `PATH` named `bin`;
4. the conventional install locations: on Windows, a directory named like
   Maxima under `C:\`, `C:\Program Files` or `C:\Program Files (x86)`, the
   newest first; elsewhere `/usr/local`, then `/usr`, then one under `/opt`.

A root is usable when it holds an SBCL runtime at `<root>/bin/sbcl` (`sbcl.exe`
on Windows) and a Maxima core at
`<root>/lib/maxima/<version>/binary-sbcl/maxima.core`. If none is found, the
first operation throws `proxima::KernelError` naming every place it looked.

## Use a particular installation

```cpp
proxima::Config config;
config.maxima_root = "C:/maxima-5.50.0";
proxima::Kernel kernel(config);
```

A root given this way is authoritative: if it is not a usable installation,
that is an error, not a reason to fall back to some other Maxima. For a
program you do not want to rebuild, set `MAXIMA_ROOT` instead.

## Ship Maxima with your application

Proxima starts SBCL with Maxima's core directly, and tells Maxima where its
own files are, so a copy of the installation in your application's directory
works as well as an installed one. Point `Config::maxima_root` at it — for
example at a `maxima` directory beside your executable.

What the copy needs:

- `bin/sbcl` (or `sbcl.exe`) — the SBCL runtime that built the core. A core
  runs only on the exact SBCL version that made it, so take both from the
  same installation.
- `bin/contrib/` — SBCL's modules.
- `lib/maxima/<version>/binary-sbcl/maxima.core` — Maxima itself.
- `share/maxima/<version>/` — what `load("…")` reads, among other things.
- On Windows, `bin/libgcc_s_seh-1.dll`: Proxima raises SBCL's memory limit
  when it is present, as Maxima's own launcher does, so that heavy packages
  load.

The official Windows installer also brings wxMaxima, other Lisps, Tcl/Tk and
documentation, none of which Proxima uses. Check a trimmed copy by running
Proxima's Maxima tests against it:

```sh
MAXIMA_ROOT=/path/to/your/copy ctest --preset linux -L maxima
```

Maxima is GPL. Shipping its binaries means offering its source to your users;
running it as a separate process, as Proxima does, keeps your own program
outside that licence. Check what this means for your case.
