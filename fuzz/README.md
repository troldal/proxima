# Fuzzing the parsers

Two targets, one for each parser written by hand:

| Target | Parser | Checks |
|---|---|---|
| `fuzz_sexpr` | `parseSExpr` and `fromMaxima`, which read every reply from Maxima | any text is read or refused with `mx::Error`; nothing crashes |
| `fuzz_parser` | `Expr::parse` | the same, and what parses prints back to text that parses to the same expression |

They are off by default. Configure with `-DMAXIMA_CPP_BUILD_FUZZERS=ON`.

## Fuzzing

libFuzzer needs Clang on Linux or macOS (MinGW's Clang has none). With it, the
targets are fuzzers and the library is built with AddressSanitizer, so use a
build directory of its own:

```sh
cmake -S . -B /tmp/mx-fuzz -G Ninja -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMAXIMA_CPP_BUILD_FUZZERS=ON
cmake --build /tmp/mx-fuzz --target fuzz_sexpr fuzz_parser
mkdir -p /tmp/mx-fuzz/found/sexpr
/tmp/mx-fuzz/fuzz/fuzz_sexpr /tmp/mx-fuzz/found/sexpr fuzz/corpus/sexpr -max_total_time=600
```

The first directory collects the inputs the fuzzer finds interesting; the
checked-in corpus seeds it and is not written to. A failing input is saved as
`crash-<hash>`. Once the bug is fixed, copy that file into `fuzz/corpus/` so it
stays fixed.

## Replaying

Everywhere else — GCC, MSVC, MinGW Clang — the same targets build as replay
programs, which run their entry point over files and directories:

```sh
fuzz_parser crash-0123abcd
```

With tests enabled, both corpora run as the CTest tests `fuzz_sexpr_corpus` and
`fuzz_parser_corpus`.
