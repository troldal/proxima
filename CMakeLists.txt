cmake_minimum_required(VERSION 3.22)
project(maxima_cpp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# All symbolic work is delegated to a Maxima child process, so nothing here
# links against a computer algebra library. The one dependency is
# Boost.Multiprecision, which backs mx::Integer: header-only, so it is compiled
# rather than linked, but it appears in a public header and so must reach
# consumers too -- see the install rules below.

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
# multiprecision and whatever it depends on -- Boost entire is far more than
# this library needs, and configuring all of it is slow.
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
        "BOOST_INCLUDE_LIBRARIES multiprecision"
)

if (WIN32)
    set(MX_CHILD_PROCESS_SOURCE src/transport/child_process_win32.cpp)
else ()
    set(MX_CHILD_PROCESS_SOURCE src/transport/child_process_posix.cpp)
endif ()

add_library(maxima_cpp
        src/core/expr.cpp
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
        src/kernel/session.cpp
        src/transport/fake_transport.cpp
        src/transport/process_env.cpp
        src/wire/from_maxima.cpp
        src/wire/sexpr.cpp
        src/wire/to_maxima.cpp
        ${MX_CHILD_PROCESS_SOURCE}
)

# PUBLIC: include/mx/integer.hpp includes <boost/multiprecision/cpp_int.hpp>,
# so anything including <mx/expr.hpp> needs Boost's headers on its path.
# Boost::multiprecision rather than Boost::headers, so the dependency says which
# part of Boost is actually used.
target_link_libraries(maxima_cpp PUBLIC Boost::multiprecision)

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
        # src/ is private: internal headers such as transport/child_process_win32
        # pull in <windows.h>, which must never reach a consumer of include/mx.
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(maxima_cpp PUBLIC cxx_std_23)

add_executable(demo examples/demo.cpp)
target_link_libraries(demo PRIVATE mx::maxima_cpp)

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
