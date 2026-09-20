# Choose, or ship, the Maxima installation

**Task:** control which Maxima your program runs, or bring one with it.

## How Proxima finds Maxima

When a kernel first starts it looks, in order, at:

1. `Config::sbcl_exe` and `Config::maxima_core`, if you set them — see
   [Name it outright](#name-it-outright), which ends the search here;
2. `Config::maxima_root`, if you set it;
3. the environment variables `MAXIMA_ROOT` and then `MAXIMA_PREFIX`;
4. the parent of any directory on `PATH` named `bin`;
5. the conventional install locations: on Windows, a directory named like
   Maxima under `C:\`, `C:\Program Files` or `C:\Program Files (x86)`, the
   newest first; elsewhere `/usr/local`, then `/usr`, then one under `/opt`.

A root is usable when it holds an SBCL runtime at `<root>/bin/sbcl` (`sbcl.exe`
on Windows) and a Maxima core. Where the core is, Proxima asks the
installation rather than assuming: it runs `<root>/bin/maxima -d`, whose
`maxima-imagesdir` names the directory holding `maxima.core`. That is the
installation's own answer, so `lib` against `lib64` and however the version
directory is spelled do not matter. A copy with no launcher to ask — a trimmed
one shipped with an application — falls back to the documented layout,
`<root>/lib/maxima/<version>/binary-sbcl/maxima.core`.

If none is found, the first operation throws `proxima::KernelError` naming
every place it looked.

## Name it outright

To leave nothing to discovery, name the two files Proxima runs:

```cpp
proxima::Config config;
config.sbcl_exe = "/opt/my-app/maxima/bin/sbcl";
config.maxima_core = "/opt/my-app/maxima/lib/maxima/5.50.0/binary-sbcl/maxima.core";
proxima::Kernel kernel(config);
```

Nothing is then searched for, no layout is assumed and no launcher is run:
those two files are the installation. Set both or neither — one without the
other is a `KernelError`, as is either one naming no file. `maxima_root` may
be set alongside them, and is then only the prefix Maxima is told about;
left empty, the prefix is the directory above SBCL's.

This is what an application shipping its own copy of Maxima wants, and an
installer that knows where it put things.

## Refuse to look elsewhere

`Config::search` says how far a kernel may go beyond what the `Config` names.
Each setting includes the ones before it:

| `Search::` | Consults |
|---|---|
| `Configured` | only what the `Config` names |
| `Environment` | and `MAXIMA_ROOT`, `MAXIMA_PREFIX`, `PATH` |
| `Automatic` | and the conventional install locations (the default) |

```cpp
proxima::Config config;
config.maxima_root = "/opt/my-app/maxima";
config.search = proxima::Search::Configured;   // that one, or an error
```

With `Search::Configured`, a program runs the Maxima it named or none at all:
nothing on the machine can stand in for it. The error says what was not
consulted, so the cause is plain.

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
example at a `maxima` directory beside your executable — or name the two
files outright, as above, which asks nothing of the copy's layout.

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
