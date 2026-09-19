# A project that uses Proxima the way another project would, built by CI in
# both of the ways a project can get it:
#
#   package  find_package against an installed Proxima:
#                cmake --install <proxima build> --prefix <prefix>
#                cmake -S tests/consumer -B <build> -DCMAKE_PREFIX_PATH=<prefix>
#   fetch    FetchContent from GitHub, as a consumer's CMakeLists would write
#            it. CI points it at the commit under test instead:
#                cmake -S tests/consumer -B <build> -DCONSUMER_FETCH=ON \
#                      -DFETCHCONTENT_SOURCE_DIR_PROXIMA=<proxima checkout>
#
# Deliberately not part of Proxima's own build, and deliberately sets no C++
# standard: the proxima::proxima target has to bring C++23 with it.

cmake_minimum_required(VERSION 3.22)
project(proxima_consumer LANGUAGES CXX)

option(CONSUMER_FETCH "Get Proxima with FetchContent rather than find_package" OFF)

if (CONSUMER_FETCH)
    include(FetchContent)
    FetchContent_Declare(proxima
            GIT_REPOSITORY https://github.com/troldal/proxima.git
            GIT_TAG master)
    FetchContent_MakeAvailable(proxima)

    # Pulled in as a subproject, Proxima builds the library and nothing else.
    foreach (target IN ITEMS demo tour functional proxima_tests)
        if (TARGET ${target})
            message(FATAL_ERROR "Proxima as a subproject defined its target '${target}'")
        endif ()
    endforeach ()
else ()
    find_package(proxima REQUIRED)
endif ()

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE proxima::proxima)

enable_testing()
add_test(NAME consumer COMMAND consumer)
