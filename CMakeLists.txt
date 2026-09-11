cmake_minimum_required(VERSION 3.22)
project(maxima_cpp CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# All symbolic work is delegated to a Maxima child process, so there are no
# third-party dependencies to build or link. cmake/CPM.cmake is kept for the
# test framework added later (see PLAN.md step 3).
if (WIN32)
    set(MX_CHILD_PROCESS_SOURCE src/transport/child_process_win32.cpp)
else ()
    set(MX_CHILD_PROCESS_SOURCE src/transport/child_process_posix.cpp)
endif ()

add_library(maxima_cpp
        src/core/expr.cpp
        src/core/printer.cpp
        src/kernel/discovery.cpp
        src/kernel/kernel.cpp
        src/kernel/session.cpp
        src/transport/fake_transport.cpp
        src/transport/process_env.cpp
        src/wire/sexpr.cpp
        ${MX_CHILD_PROCESS_SOURCE}
)

if (NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(maxima_cpp PRIVATE Threads::Threads)
endif ()
add_library(mx::maxima_cpp ALIAS maxima_cpp)

target_include_directories(maxima_cpp
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
        # src/ is private: internal headers such as kernel/session.hpp pull in
        # <windows.h>, which must never reach a consumer of include/mx.
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

add_executable(demo examples/demo.cpp)
target_link_libraries(demo PRIVATE mx::maxima_cpp)

option(MAXIMA_CPP_BUILD_TESTS "Build the maxima_cpp test suite"
        ${PROJECT_IS_TOP_LEVEL})

if (MAXIMA_CPP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif ()
