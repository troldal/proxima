cmake_minimum_required(VERSION 3.22)
project(sym_test)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(cmake/CPM.cmake)

# GMP's official build system does not support MSVC, so its prebuilt MinGW
# static libs can't be linked from an MSVC executable (different runtime/ABI,
# e.g. missing __chkstk_ms / __mingw_vfprintf). For MSVC we therefore use
# SymEngine's "boostmp" integer backend (Boost.Multiprecision, header-only)
# instead of GMP. Boost.Multiprecision was installed via vcpkg:
#   C:/Users/kenne/vcpkg/vcpkg.exe install boost-multiprecision:x64-windows
if (MSVC)
    set(SYM_TEST_INTEGER_CLASS "boostmp")
    set(VCPKG_INSTALLED_DIR "C:/Users/kenne/vcpkg/installed/x64-windows")
    list(APPEND CMAKE_PREFIX_PATH "${VCPKG_INSTALLED_DIR}")
else()
    set(SYM_TEST_INTEGER_CLASS "gmp")

    # SymEngine needs GMP. Point it at the GMP headers/libs bundled with
    # Strawberry Perl's MinGW toolchain (adjust if using a different GMP install).
    set(GMP_INCLUDE_DIR "C:/Strawberry/c/include" CACHE PATH "")
    set(GMP_LIBRARY "C:/Strawberry/c/lib/libgmp.a" CACHE FILEPATH "")
endif()

CPMAddPackage(
    NAME symengine
    GITHUB_REPOSITORY symengine/symengine
    GIT_TAG v0.14.0
    OPTIONS
        "BUILD_TESTS OFF"
        "BUILD_BENCHMARKS OFF"
        "INTEGER_CLASS ${SYM_TEST_INTEGER_CLASS}"
        # Force SymEngine's own RCP implementation instead of Teuchos::RCP.
        # SymEngine only defaults to this in Release builds; without it,
        # Debug builds require Teuchos (which isn't well supported on MinGW).
        "WITH_SYMENGINE_RCP ON"
)

add_executable(sym_test main.cpp)
target_link_libraries(sym_test PRIVATE symengine)
target_include_directories(sym_test PRIVATE
    ${symengine_SOURCE_DIR}
    ${symengine_BINARY_DIR}
)
if (MSVC)
    target_include_directories(sym_test PRIVATE "${VCPKG_INSTALLED_DIR}/include")
else()
    target_include_directories(sym_test PRIVATE ${GMP_INCLUDE_DIR})
endif()
