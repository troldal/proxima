cmake_minimum_required(VERSION 3.22)
project(maxima_cpp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# All symbolic work is delegated to a Maxima child process, so nothing here
# links against a computer algebra library. Two parts of Boost are used:
#
#   Boost.Multiprecision  backs mx::Integer. Header-only, but it appears in a
#                         public header, so it must reach consumers too -- see
#                         the install rules below.
#   Boost.Process (v2)    starts and talks to the Maxima child, over Boost.Asio.
#                         A compiled library, and private: nothing about it
#                         appears in include/mx.

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
        set(_mx_cpm_cache "$ENV{USERPROFILE}/.cache/CPM")
    elseif (DEFINED ENV{HOME})
        set(_mx_cpm_cache "$ENV{HOME}/.cache/CPM")
    endif ()
    if (_mx_cpm_cache)
        set(CPM_SOURCE_CACHE "${_mx_cpm_cache}" CACHE PATH
                "Directory CPM unpacks dependency sources into, shared between build trees"
                FORCE)
        unset(_mx_cpm_cache)
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
# fetched dependency. mx::Integer holds a cpp_int by value, so the type is in
# <mx/integer.hpp> and every consumer needs those headers; skipping the install
# rules would produce an installed library that cannot be included. This is the
# opposite of the doctest case in tests/CMakeLists.txt, where a test framework
# has no business in a consumer's prefix.
#
# 1.92.0 and not something older: Boost.Container's bundled dlmalloc passed an
# `int*` to InterlockedExchange, which wants `volatile long*`. Clang has treated
# that as an error rather than a warning since 16, so clang-cl could not build
# boost_container -- a library nothing here uses, dragged in because
# Boost::multiprecision declares Boost::random, which depends on it. Upstream
# added the cast in 1.89. Do not pin this below that.
CPMAddPackage(
        NAME Boost
        VERSION 1.92.0
        URL https://github.com/boostorg/boost/releases/download/boost-1.92.0/boost-1.92.0-cmake.tar.xz
        URL_HASH SHA256=9bed76128d4e46755dbe818487788c6fceb6f72b378f4daa49b7e1e600d9088d
        SYSTEM YES
        OPTIONS
        "BOOST_ENABLE_CMAKE ON"
        "BOOST_SKIP_INSTALL_RULES OFF"
        "BUILD_SHARED_LIBS OFF"
        "BOOST_INCLUDE_LIBRARIES multiprecision\\\;process"
        "BOOST_PROCESS_USE_STD_FS ON"
)

add_library(maxima_cpp
        src/core/expr.cpp
        src/core/format.cpp
        src/core/integer.cpp
        src/core/normalize.cpp
        src/core/numeric.cpp
        src/core/parser.cpp
        src/core/printer.cpp
        src/core/render.cpp
        src/core/tex.cpp
        src/core/mathml.cpp
        src/kernel/cache.cpp
        src/kernel/context.cpp
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

# PUBLIC: include/mx/integer.hpp includes <boost/multiprecision/cpp_int.hpp>,
# so anything including <mx/expr.hpp> needs Boost's headers on its path.
# Boost::multiprecision rather than Boost::headers, so the dependency says which
# part of Boost is actually used.
target_link_libraries(maxima_cpp PUBLIC Boost::multiprecision)

# PRIVATE: only src/transport/child_process.cpp includes it. The library is
# static, though, so consumers still link it; the installed package config
# finds it for them.
target_link_libraries(maxima_cpp PRIVATE Boost::process)

if (WIN32)
    # Asio names its pipes using BCryptGenRandom, and Boost::process does not
    # carry bcrypt in its link interface; the MinGW linker does not find it
    # by itself.
    target_link_libraries(maxima_cpp PRIVATE bcrypt)
endif ()

if (NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(maxima_cpp PRIVATE Threads::Threads)
endif ()
add_library(mx::maxima_cpp ALIAS maxima_cpp)

# Generated so the version cannot drift from the one declared above.
configure_file(cmake/version.hpp.in
        "${CMAKE_CURRENT_BINARY_DIR}/generated/mx/version.hpp" @ONLY)

target_include_directories(maxima_cpp
        PUBLIC
        # Two paths for the same headers: the source tree when built as part of
        # a larger project, the install prefix when consumed through
        # find_package.
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
        $<INSTALL_INTERFACE:include>
        # src/ is private: the transport pulls in Boost.Asio and, through it,
        # <windows.h>, neither of which may reach a consumer of include/mx.
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(maxima_cpp PUBLIC cxx_std_23)

# Every source file here is UTF-8 without a byte-order mark. MSVC reads such a
# file in the ANSI code page unless told otherwise, so a non-ASCII character in
# a string literal compiles to the UTF-8 of different characters; GCC and Clang,
# clang-cl included, assume UTF-8 already. PRIVATE, and on each target that
# compiles these sources, because it describes this project's files and not a
# consumer's.
set(MX_MSVC_UTF8 $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_compile_options(maxima_cpp PRIVATE ${MX_MSVC_UTF8})

add_executable(demo examples/demo.cpp)
target_link_libraries(demo PRIVATE mx::maxima_cpp)
target_compile_options(demo PRIVATE ${MX_MSVC_UTF8})

# A longer, heavily commented walk through every public feature.
add_executable(tour examples/tour.cpp)
target_link_libraries(tour PRIVATE mx::maxima_cpp)
target_compile_options(tour PRIVATE ${MX_MSVC_UTF8})

# --- Installation ---------------------------------------------------------

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS maxima_cpp
        EXPORT maxima_cppTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY include/mx DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/generated/mx/version.hpp"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/mx)

install(EXPORT maxima_cppTargets
        FILE maxima_cppTargets.cmake
        NAMESPACE mx::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/maxima_cpp
)

configure_package_config_file(
        cmake/maxima_cppConfig.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/maxima_cppConfig.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/maxima_cpp
)
write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/maxima_cppConfigVersion.cmake"
        COMPATIBILITY SameMajorVersion
)
install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/maxima_cppConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/maxima_cppConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/maxima_cpp
)

# --- Tests ----------------------------------------------------------------

option(MAXIMA_CPP_BUILD_TESTS "Build the maxima_cpp test suite"
        ${PROJECT_IS_TOP_LEVEL})

if (MAXIMA_CPP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif ()
