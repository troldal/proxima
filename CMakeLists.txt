cmake_minimum_required(VERSION 3.22)
project(maxima_cpp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# All symbolic work is delegated to a Maxima child process, so nothing here
# links against a computer algebra library. The one dependency is
# Boost.Multiprecision, which backs mx::Integer -- header-only, so it is
# compiled rather than linked, but it appears in a public header and so must
# reach consumers too. cmake/CPM.cmake is kept for the test framework
# (see PLAN.md step 3).
#
# Boost is found the ordinary way: a system package on Linux, or a toolchain
# file or CMAKE_PREFIX_PATH anywhere. Windows has no system package to find, so
# failing that, a vcpkg tree in one of its usual places is used -- which is what
# lets `cmake --preset windows` work with no further setup. Set VCPKG_ROOT, or
# pass vcpkg's toolchain file, to point this somewhere else.
find_package(Boost QUIET)
if (NOT Boost_FOUND AND WIN32)
    foreach (vcpkg_root "$ENV{VCPKG_ROOT}" "$ENV{USERPROFILE}/vcpkg" "C:/vcpkg")
        if (vcpkg_root AND IS_DIRECTORY "${vcpkg_root}/installed")
            file(GLOB vcpkg_trees "${vcpkg_root}/installed/*")
            list(APPEND CMAKE_PREFIX_PATH ${vcpkg_trees})
            break()
        endif ()
    endforeach ()
endif ()
find_package(Boost REQUIRED)

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
        ${MX_CHILD_PROCESS_SOURCE}
)

# PUBLIC: include/mx/integer.hpp includes <boost/multiprecision/cpp_int.hpp>,
# so anything including <mx/expr.hpp> needs Boost's headers on its path.
target_link_libraries(maxima_cpp PUBLIC Boost::headers)

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
