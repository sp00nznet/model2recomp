cmake_minimum_required(VERSION 3.20)
project(model2recomp C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Find SDL2
find_package(SDL2 REQUIRED)

# --- model2recomp static library ---
add_library(model2recomp STATIC
    src/model2recomp.c
    src/i960.c
    src/bus.c
    src/func_table.c
    src/video.c
    src/geometry.c
    src/sound.c
    src/io.c
    src/timer.c
    src/eeprom.c
    src/platform_sdl.c
)

target_include_directories(model2recomp
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(model2recomp
    PUBLIC SDL2::SDL2
)

# MSVC settings
if(MSVC)
    target_compile_options(model2recomp PRIVATE /W3)
    target_compile_definitions(model2recomp PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(model2recomp PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# --- Optional: minimal example ---
option(MODEL2RECOMP_BUILD_EXAMPLES "Build example programs" ON)
if(MODEL2RECOMP_BUILD_EXAMPLES)
    add_executable(model2recomp_minimal
        examples/minimal/main.c
    )
    target_link_libraries(model2recomp_minimal PRIVATE
        model2recomp
        SDL2::SDL2main
    )
endif()

# --- Tests ---
option(MODEL2RECOMP_BUILD_TESTS "Build test programs" ON)
if(MODEL2RECOMP_BUILD_TESTS)
    add_executable(model2recomp_test_tilemap
        tests/test_tilemap.c
    )
    target_link_libraries(model2recomp_test_tilemap PRIVATE
        model2recomp
        SDL2::SDL2main
    )

    add_executable(model2recomp_test_geometry
        tests/test_geometry.c
    )
    target_link_libraries(model2recomp_test_geometry PRIVATE
        model2recomp
        SDL2::SDL2main
    )
endif()
