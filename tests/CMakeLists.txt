include(${CMAKE_SOURCE_DIR}/cmake/CPM.cmake)

CPMAddPackage(
        NAME doctest
        GITHUB_REPOSITORY doctest/doctest
        GIT_TAG v2.4.12
        EXCLUDE_FROM_ALL YES
        SYSTEM YES
        OPTIONS
        "DOCTEST_WITH_TESTS OFF"
        # Without this, `cmake --install` on this project would also install
        # doctest's headers and package config into the consumer's prefix. A
        # test framework is our business, not theirs.
        "DOCTEST_NO_INSTALL ON"
)

add_executable(proxima_tests
        test_main.cpp
        test_assumptions.cpp
        test_basics.cpp
        test_cache.cpp
        test_discovery.cpp
        test_expr.cpp
        test_format.cpp
        test_from_maxima.cpp
        test_integer.cpp
        test_normalize.cpp
        test_numeric.cpp
        test_order.cpp
        test_ops.cpp
        test_parser.cpp
        test_persistent_cache.cpp
        test_process_env.cpp
        test_properties.cpp
        test_public_headers.cpp
        test_render.cpp
        test_sexpr.cpp
        test_to_maxima.cpp
        test_kernel.cpp
        test_transport.cpp
        test_traverse.cpp
)
target_link_libraries(proxima_tests PRIVATE proxima::proxima doctest::doctest)

# The sources are UTF-8, and held to the same warnings as the library; see
# PROXIMA_MSVC_UTF8 and PROXIMA_WARNINGS in the top-level CMakeLists.txt.
target_compile_options(proxima_tests PRIVATE ${PROXIMA_MSVC_UTF8} ${PROXIMA_WARNINGS})

# White-box access: the transport and session layers are internal, but they are
# exactly what the Maxima-free tests exercise.
target_include_directories(proxima_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)

# The recorded Maxima replies the reader and mapping layers are tested against.
target_compile_definitions(proxima_tests PRIVATE
        PROXIMA_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")

# Two CTest entries over one binary, split by doctest test suite. Tests that
# need a working Maxima installation are labelled so they can be excluded:
#
#     ctest -LE maxima      # everything that runs without Maxima
#
# The Maxima-free half covers the protocol, reader and term layers through
# FakeTransport, so this split is what keeps most of the suite runnable in CI.
add_test(NAME unit COMMAND proxima_tests --test-suite-exclude=maxima)

add_test(NAME integration COMMAND proxima_tests --test-suite=maxima)
set_tests_properties(integration PROPERTIES
        LABELS "maxima"
        TIMEOUT 120
)
