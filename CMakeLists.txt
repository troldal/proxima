cmake_minimum_required(VERSION 3.22)
project(proxima VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# No C++20 modules here or in the dependencies. CMake 3.28 and later scan every
# C++20 target for them unless told not to, and with clang that needs
# clang-scan-deps, which a distribution may package separately from the
# compiler: Ubuntu's clang-19 alone failed to build Boost.Process for want of it.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# All symbolic work is delegated to a Maxima child process, so nothing here
# links against a computer algebra library. Two parts of Boost are used:
#
#   Boost.Multiprecision  backs proxima::Integer. Header-only, but it appears in a
#                         public header, so it must reach consumers too -- see
#                         the install rules below.
#   Boost.Process (v2)    starts and talks to the Maxima child, over Boost.Asio.
#                         A compiled library, and private: nothing about it
#                         appears in include/proxima.

# Boost is a large download, and a copy per build directory would be several
# gigabytes across the presets in CMakePresets.json. CPM keeps one shared source
# cache instead; an explicit CPM_SOURCE_CACHE, in the cache or the environment,
# is left alone.
# Note the plain falsiness test rather than `NOT DEFINED`: CPM declares
# CPM_SOURCE_CACHE itself and leaves it OFF, so by the second configure of a
# build tree it is always *defined* -- and a `NOT DEFINED` guard here silently
# never fires, which is how 732 MB of Boost ends up in _deps.
if (NOT CPM_SOURCE_CACHE AND NOT DEFINED ENV{CPM_SOURCE_CACHE})
    if (DEFINED ENV{USERPROFILE})
        set(_proxima_cpm_cache "$ENV{USERPROFILE}/.cache/CPM")
    elseif (DEFINED ENV{HOME})
        set(_proxima_cpm_cache "$ENV{HOME}/.cache/CPM")
    endif ()
    if (_proxima_cpm_cache)
        set(CPM_SOURCE_CACHE "${_proxima_cpm_cache}" CACHE PATH
                "Directory CPM unpacks dependency sources into, shared between build trees"
                FORCE)
        unset(_proxima_cpm_cache)
    endif ()
endif ()
message(STATUS "CPM source cache: ${CPM_SOURCE_CACHE}")

include(cmake/CPM.cmake)

# The CMake-native Boost release rather than the historical one: BOOST_ENABLE_CMAKE
# builds it as an ordinary CMake project, so there is no b2 bootstrap and no
# separate build step. BOOST_INCLUDE_LIBRARIES narrows the superbuild to
# multiprecision, process and whatever they depend on -- Boost entire is far
# more than this library needs, and configuring all of it is slow. The list
# separator is escaped for CPM, which would otherwise split it into two options.
#
# BOOST_PROCESS_USE_STD_FS has Boost.Process use std::filesystem, so it does
# not also need Boost.Filesystem linked in.
#
# BOOST_SKIP_INSTALL_RULES is OFF deliberately, against the usual advice for a
# fetched dependency. Proxima is a static library linking Boost.Process's
# compiled part, so every consumer links that too; skipping the install rules
# would produce an installed library that cannot be linked. (It used to be the
# headers as well: proxima::Integer held a cpp_int by value in its public
# header. It no longer does, and no public header includes Boost.) This is the
# opposite of the doctest case in tests/CMakeLists.txt, where a test framework
# has no business in a consumer's prefix.
#
# 1.92.0 and not something older: Boost.Container's bundled dlmalloc passed an
# `int*` to InterlockedExchange, which wants `volatile long*`. Clang has treated
# that as an error rather than a warning since 16, so clang-cl could not build
# boost_container -- a library nothing here uses, dragged in because
# Boost::multiprecision declares Boost::random, which depends on it. Upstream
# added the cast in 1.89. Do not pin this below that.
# BUILD_SHARED_LIBS OFF for Boost alone: static, as the install rules above
# expect. It used to be one of CPM's OPTIONS, which CPM sets as a cache
# variable, so it silently became this project's default too. A normal
# variable reaches Boost's subdirectory and goes away again below, leaving any
# value the user set.
set(BUILD_SHARED_LIBS OFF)
CPMAddPackage(
        NAME Boost
        VERSION 1.92.0
        URL https://github.com/boostorg/boost/releases/download/boost-1.92.0/boost-1.92.0-cmake.tar.xz
        URL_HASH SHA256=9bed76128d4e46755dbe818487788c6fceb6f72b378f4daa49b7e1e600d9088d
        SYSTEM YES
        OPTIONS
        "BOOST_ENABLE_CMAKE ON"
        "BOOST_SKIP_INSTALL_RULES OFF"
        "BOOST_INCLUDE_LIBRARIES multiprecision\\\;process"
        "BOOST_PROCESS_USE_STD_FS ON"
)
unset(BUILD_SHARED_LIBS)

# FXT, the functional extensions this library's result type is built on:
# proxima::Failure is fxt::failure and proxima::result<T> is fxt::result<T>,
# so a Proxima result takes part in any FXT pipeline directly. Header-only,
# and DOWNLOAD_ONLY: FXT's own CMakeLists fetches a second CPM and the
# TartanLlama fallbacks for std::expected and std::optional, none of which a
# C++23 build needs, so the interface target is declared here instead.
# fixed_string is the one header FXT's string enums include, fetched so that
# <fxt.hpp> compiles for a consumer as it does for FXT itself.
CPMAddPackage(
        NAME fxt
        GITHUB_REPOSITORY troldal/FXT
        GIT_TAG 9208e597156b196dd5cbf55ab8460ffb2cead30b
        DOWNLOAD_ONLY YES
)
CPMAddPackage(
        NAME fixed_string
        GITHUB_REPOSITORY unterumarmung/fixed_string
        GIT_TAG v0.1.1
        DOWNLOAD_ONLY YES
)
add_library(fxt INTERFACE)
target_include_directories(fxt SYSTEM INTERFACE
        $<BUILD_INTERFACE:${fxt_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${fixed_string_SOURCE_DIR}/include>
)
target_compile_features(fxt INTERFACE cxx_std_23)
add_library(fxt::fxt ALIAS fxt)

add_library(proxima
        src/core/assumptions.cpp
        src/core/expr.cpp
        src/core/format.cpp
        src/core/integer.cpp
        src/core/normalize.cpp
        src/core/numeric.cpp
        src/core/parser.cpp
        src/core/printer.cpp
        src/core/render.cpp
        src/core/tex.cpp
        src/core/traverse.cpp
        src/core/mathml.cpp
        src/kernel/cache.cpp
        src/kernel/discovery.cpp
        src/kernel/persistent_cache.cpp
        src/kernel/kernel.cpp
        src/kernel/ops.cpp
        src/kernel/sbcl_path.cpp
        src/kernel/session.cpp
        src/transport/child_process.cpp
        src/transport/fake_transport.cpp
        src/transport/process_env.cpp
        src/wire/from_maxima.cpp
        src/wire/sexpr.cpp
        src/wire/to_maxima.cpp
)

# PUBLIC: <proxima/result.hpp> includes FXT's Expected.hpp and Failure.hpp,
# and so does every header that reports a failure. In the build tree the
# include directories come from the fxt target above; for an installed
# library, FXT's headers are installed beside this library's (see below) and
# so are found through the same include directory, with no target to export.
target_link_libraries(proxima PUBLIC $<BUILD_INTERFACE:fxt::fxt>)

# PRIVATE, and only while building: multiprecision is header-only, and only
# src/core/integer.cpp and the normaliser include it, through
# src/core/big_int.hpp. proxima::Integer names its large-value type without
# defining it, so no public header needs Boost. BUILD_INTERFACE keeps the
# target out of the exported link interface, so an installed Proxima does not
# ask its consumers for it. It used to be PUBLIC, and cost every consumer
# translation unit 118,000 preprocessed lines.
target_link_libraries(proxima PRIVATE $<BUILD_INTERFACE:Boost::multiprecision>)

# PRIVATE: only src/transport/child_process.cpp includes it. The library is
# static, though, so consumers still link it; the installed package config
# finds it for them.
target_link_libraries(proxima PRIVATE Boost::process)

if (WIN32)
    # Asio names its pipes using BCryptGenRandom, and Boost::process does not
    # carry bcrypt in its link interface; the MinGW linker does not find it
    # by itself.
    target_link_libraries(proxima PRIVATE bcrypt)
endif ()

if (NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(proxima PRIVATE Threads::Threads)
endif ()
add_library(proxima::proxima ALIAS proxima)

# Generated so the version cannot drift from the one declared above.
configure_file(cmake/version.hpp.in
        "${CMAKE_CURRENT_BINARY_DIR}/generated/proxima/version.hpp" @ONLY)

target_include_directories(proxima
        PUBLIC
        # Two paths for the same headers: the source tree when built as part of
        # a larger project, the install prefix when consumed through
        # find_package.
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
        $<INSTALL_INTERFACE:include>
        # src/ is private: the transport pulls in Boost.Asio and, through it,
        # <windows.h>, neither of which may reach a consumer of include/proxima.
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(proxima PUBLIC cxx_std_23)

# Every source file here is UTF-8 without a byte-order mark. MSVC reads such a
# file in the ANSI code page unless told otherwise, so a non-ASCII character in
# a string literal compiles to the UTF-8 of different characters; GCC and Clang,
# clang-cl included, assume UTF-8 already. PRIVATE, and on each target that
# compiles these sources, because it describes this project's files and not a
# consumer's.
set(PROXIMA_MSVC_UTF8 $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

# Warnings, for this project's own code. The sources compile cleanly under all
# of these, with GCC, Clang, clang-cl and MSVC; turning them on keeps it so.
# Boost and doctest are SYSTEM includes, so their headers are exempt. PRIVATE
# on each target, like PROXIMA_MSVC_UTF8: they describe this project's code, not a
# consumer's. With PROXIMA_WARNINGS_AS_ERRORS, a warning fails the build.
option(PROXIMA_WARNINGS_AS_ERRORS
        "Treat warnings in Proxima's own code as errors" OFF)
if (MSVC)
    # /W4 for MSVC and for clang-cl, which maps it to -Wall -Wextra.
    set(PROXIMA_WARNINGS /W4)
    if (PROXIMA_WARNINGS_AS_ERRORS)
        list(APPEND PROXIMA_WARNINGS /WX)
    endif ()
else ()
    set(PROXIMA_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion
            -Wsign-conversion -Wold-style-cast)
    if (PROXIMA_WARNINGS_AS_ERRORS)
        list(APPEND PROXIMA_WARNINGS -Werror)
    endif ()
endif ()

target_compile_options(proxima PRIVATE ${PROXIMA_MSVC_UTF8} ${PROXIMA_WARNINGS})

add_executable(demo examples/demo.cpp)
target_link_libraries(demo PRIVATE proxima::proxima)
target_compile_options(demo PRIVATE ${PROXIMA_MSVC_UTF8} ${PROXIMA_WARNINGS})

# A longer, heavily commented walk through every public feature.
add_executable(tour examples/tour.cpp)
target_link_libraries(tour PRIVATE proxima::proxima)
target_compile_options(tour PRIVATE ${PROXIMA_MSVC_UTF8} ${PROXIMA_WARNINGS})

# Proxima in a functional style: its results composed with FXT's adaptors.
add_executable(functional examples/functional.cpp)
target_link_libraries(functional PRIVATE proxima::proxima)
target_compile_options(functional PRIVATE ${PROXIMA_MSVC_UTF8} ${PROXIMA_WARNINGS})

# --- Installation ---------------------------------------------------------

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS proxima
        EXPORT proximaTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY include/proxima DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# FXT's headers go with this library's: the public result type is FXT's, so
# an installed <proxima/result.hpp> that could not find <fxt/...> would not
# compile. Installed unversioned, under the names FXT's own includes use, so
# a consumer's `#include <fxt.hpp>` resolves too. A consumer carrying its own
# FXT sees whichever include directory comes first; the two are the same
# headers at the commit pinned above.
install(DIRECTORY "${fxt_SOURCE_DIR}/include/" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY "${fixed_string_SOURCE_DIR}/include/" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/generated/proxima/version.hpp"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/proxima)

install(EXPORT proximaTargets
        FILE proximaTargets.cmake
        NAMESPACE proxima::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/proxima
)

configure_package_config_file(
        cmake/proximaConfig.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/proximaConfig.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/proxima
)
write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/proximaConfigVersion.cmake"
        COMPATIBILITY SameMajorVersion
)
install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/proximaConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/proximaConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/proxima
)

# --- Tests ----------------------------------------------------------------

option(PROXIMA_BUILD_TESTS "Build the Proxima test suite"
        ${PROJECT_IS_TOP_LEVEL})

if (PROXIMA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif ()

# --- Fuzzing --------------------------------------------------------------

# Fuzz targets for the two hand-written parsers; see fuzz/README.md. Off by
# default: with a Clang that has libFuzzer they instrument the whole library.
option(PROXIMA_BUILD_FUZZERS "Build fuzz targets for Proxima's parsers" OFF)

if (PROXIMA_BUILD_FUZZERS)
    add_subdirectory(fuzz)
endif ()
