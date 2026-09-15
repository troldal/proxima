# Fuzz targets for the two parsers written by hand: parseSExpr, which reads
# every reply from Maxima, and Expr::parse, which reads user text. fuzz/README.md
# says how to run them.
#
# With a Clang that has libFuzzer, each target is a fuzzer, and the library is
# instrumented for it and for AddressSanitizer. Anywhere else — GCC, MSVC, and
# MinGW's Clang, which has no libFuzzer — each builds as a replay program over
# the same entry point, so an input a fuzzer found on one machine reproduces on
# any other.

set(PROXIMA_HAVE_LIBFUZZER OFF)
if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
        AND NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer")
    check_cxx_source_compiles([=[
        #include <cstddef>
        #include <cstdint>
        extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *, std::size_t) {
            return 0;
        }
    ]=] PROXIMA_LIBFUZZER_LINKS)
    unset(CMAKE_REQUIRED_FLAGS)
    set(PROXIMA_HAVE_LIBFUZZER ${PROXIMA_LIBFUZZER_LINKS})
endif ()
message(STATUS "Fuzz targets: ${PROXIMA_HAVE_LIBFUZZER} for libFuzzer, otherwise replay programs")

if (PROXIMA_HAVE_LIBFUZZER)
    # The library too: coverage from inside the parsers is what guides the
    # fuzzer, and the sanitizer is what turns a quiet overflow into a report.
    target_compile_options(proxima PRIVATE -fsanitize=fuzzer-no-link,address)
endif ()

foreach (target IN ITEMS fuzz_sexpr fuzz_parser)
    if (PROXIMA_HAVE_LIBFUZZER)
        add_executable(${target} ${target}.cpp)
        target_compile_options(${target} PRIVATE -fsanitize=fuzzer,address)
        target_link_options(${target} PRIVATE -fsanitize=fuzzer,address)
    else ()
        add_executable(${target} ${target}.cpp replay_main.cpp)
    endif ()
    target_link_libraries(${target} PRIVATE proxima::proxima)
    # White-box, as the tests are: parseSExpr and fromMaxima are internal.
    target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_compile_options(${target} PRIVATE ${PROXIMA_MSVC_UTF8} ${PROXIMA_WARNINGS})
endforeach ()

if (PROXIMA_BUILD_TESTS)
    # The seeds, and every input a fuzzer has found worth keeping, run as
    # tests: a parser change that breaks one is caught without fuzzing.
    # -runs=0 has libFuzzer run the corpus once and stop; the replay program
    # ignores options.
    add_test(NAME fuzz_sexpr_corpus
             COMMAND fuzz_sexpr -runs=0 ${CMAKE_CURRENT_SOURCE_DIR}/corpus/sexpr)
    add_test(NAME fuzz_parser_corpus
             COMMAND fuzz_parser -runs=0 ${CMAKE_CURRENT_SOURCE_DIR}/corpus/parser)
endif ()
