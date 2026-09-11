include(${CMAKE_SOURCE_DIR}/cmake/CPM.cmake)

CPMAddPackage(
        NAME doctest
        GITHUB_REPOSITORY doctest/doctest
        GIT_TAG v2.4.12
        OPTIONS
        "DOCTEST_WITH_TESTS OFF"
)

add_executable(mx_tests
        test_main.cpp
        test_basics.cpp
        test_discovery.cpp
        test_expr.cpp
        test_from_maxima.cpp
        test_normalize.cpp
        test_process_env.cpp
        test_sexpr.cpp
        test_kernel.cpp
        test_transport.cpp
)
target_link_libraries(mx_tests PRIVATE mx::maxima_cpp doctest::doctest)

# White-box access: the transport and session layers are internal, but they are
# exactly what the Maxima-free tests exercise.
target_include_directories(mx_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)

# The recorded Maxima replies the reader and mapping layers are tested against.
target_compile_definitions(mx_tests PRIVATE
        MX_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")

# Two CTest entries over one binary, split by doctest test suite. Tests that
# need a working Maxima installation are labelled so they can be excluded:
#
#     ctest -LE maxima      # everything that runs without Maxima
#
# From PLAN.md step 4 onward the Maxima-free half grows to cover the protocol,
# reader and term layers via FakeTransport, so this split is what keeps the
# suite runnable in CI.
add_test(NAME unit COMMAND mx_tests --test-suite-exclude=maxima)

add_test(NAME integration COMMAND mx_tests --test-suite=maxima)
set_tests_properties(integration PROPERTIES
        LABELS "maxima"
        TIMEOUT 120
)
