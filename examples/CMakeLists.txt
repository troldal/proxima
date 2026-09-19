# The example programs, built with PROXIMA_BUILD_EXAMPLES: by default only when
# Proxima is the top-level project, so that a project pulling it in with
# add_subdirectory, FetchContent or CPM builds the library alone.

# A one-screen showcase.
add_executable(demo demo.cpp)

# A longer, heavily commented walk through every public feature.
add_executable(tour tour.cpp)

# Proxima in a functional style: its results composed with FXT's adaptors.
add_executable(functional functional.cpp)

foreach (example IN ITEMS demo tour functional)
    target_link_libraries(${example} PRIVATE proxima::proxima)
    target_compile_options(${example} PRIVATE ${PROXIMA_MSVC_UTF8} ${PROXIMA_WARNINGS})
    # The examples print with std::print. MinGW's libstdc++ keeps the part of
    # it that writes to a console in a separate library, libstdc++exp, which
    # GCC on Windows must be told to link (GCC 16 included). libc++ and the
    # MSVC STL need nothing.
    if (MINGW AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_link_libraries(${example} PRIVATE stdc++exp)
    endif ()
endforeach ()
