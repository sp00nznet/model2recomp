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
    src/copro.c
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

# --- Corpus sweep -------------------------------------------------------------
#
# Point MODEL2RECOMP_CORPUS_DIR at a tree of lifted sets (tools/corpus.py builds
# it) and every set becomes a corpus_<set> executable sharing examples/corpus's
# generic launcher. This is how the whole library gets swept in one run instead
# of one hand-written game project per title.
#
# The lifted C is derived from a ROM and is never committed or distributed; the
# corpus directory is local scratch.
set(MODEL2RECOMP_CORPUS_DIR "" CACHE PATH "Tree of lifted Model 2 sets to build")
if(MODEL2RECOMP_CORPUS_DIR)
    file(GLOB _corpus_sets RELATIVE "${MODEL2RECOMP_CORPUS_DIR}"
         "${MODEL2RECOMP_CORPUS_DIR}/*")
    foreach(_set ${_corpus_sets})
        set(_dir "${MODEL2RECOMP_CORPUS_DIR}/${_set}")
        if(NOT EXISTS "${_dir}/recomp/game_register.c")
            continue()
        endif()
        # CONFIGURE_DEPENDS: a re-lift can emit a different number of files, and
        # a stale glob links declarations with no bodies behind them.
        file(GLOB _src CONFIGURE_DEPENDS "${_dir}/recomp/game_code_*.c")
        add_executable(corpus_${_set}
            "${_dir}/recomp/game_register.c" ${_src}
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/corpus/main.c")
        target_include_directories(corpus_${_set} PRIVATE "${_dir}/include")
        target_link_libraries(corpus_${_set} PRIVATE model2recomp SDL2::SDL2main)
        # Lifted code sometimes expresses a loop as recursion - the lifter can
        # split a routine into one-instruction fragments that tail-call each
        # other - so the dispatch chain gets deep. Reserve (do not commit) a
        # large stack; the default 1 MB is exhausted by a few thousand frames.
        if(MSVC)
            target_link_options(corpus_${_set} PRIVATE /STACK:268435456)
        endif()
        if(MSVC)
            target_compile_options(corpus_${_set} PRIVATE /W0)
            target_compile_definitions(corpus_${_set} PRIVATE _CRT_SECURE_NO_WARNINGS)
        else()
            target_compile_options(corpus_${_set} PRIVATE -w)
        endif()
        set_target_properties(corpus_${_set} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/corpus"
            RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/corpus"
            RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/corpus")
    endforeach()
endif()
