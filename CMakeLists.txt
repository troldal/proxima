cmake_minimum_required(VERSION 3.22)
project(sym_test CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# All symbolic work is delegated to a Maxima child process, so there are no
# third-party dependencies to build or link. cmake/CPM.cmake is kept for the
# test framework added later (see PLAN.md step 3).
add_executable(sym_test main.cpp)
