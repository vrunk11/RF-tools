cmake_minimum_required(VERSION 3.16)

# Enforce out-of-source builds before project() to catch issues early
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not allowed. Please create a build directory and run:
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo")
endif()

project(rf-tools VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# All executables go to build/bin, all libraries to build/lib
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# The predictor of the .rfx format gives the same bits on every machine only
# if multiply and add are never fused (docs/Specifications/rfx-format.md, 5).
if(NOT MSVC)
    add_compile_options(-ffp-contract=off)
endif()

# Where the build comes from, shown by the tools (set by the Nix and CI builds)
set(APP_BRANCH "local" CACHE STRING "Branch the build comes from")
set(APP_COMMIT "unknown" CACHE STRING "Commit or version the build comes from")


# Machine-specific optimisation for local builds: link-time optimisation plus
# code tuned for the building CPU (-march=native, or -mcpu=native where only
# that is understood). Off by default so CI, Nix and the release packages stay
# portable to any x86-64 / arm64 machine; build.sh and build.bat turn it on.
# Binaries built with it may not run on an older or different CPU.
option(ENABLE_NATIVE_OPTIMIZATION
    "Optimise for the building machine (LTO + native CPU tuning); not portable"
    OFF
)

if(ENABLE_NATIVE_OPTIMIZATION)
    include(CheckIPOSupported)
    include(CheckCXXCompilerFlag)

    check_ipo_supported(RESULT _ipo_supported OUTPUT _ipo_output LANGUAGES CXX)
    if(_ipo_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
        message(STATUS "Native optimisation: link-time optimisation enabled")
    else()
        message(STATUS "Native optimisation: LTO not supported by this toolchain")
    endif()

    # MSVC has no "native" target; it keeps its default instruction set.
    if(NOT MSVC)
        check_cxx_compiler_flag("-march=native" _have_march_native)
        if(_have_march_native)
            add_compile_options(-march=native)
            message(STATUS "Native optimisation: -march=native")

            # GCC for Windows (MinGW-w64) keeps the stack aligned to 16 bytes only, but with AVX it stores 32 or 64 bytes with aligned moves to
            # its stack (GCC bug 54412): a program built for an AVX CPU then stops with an access violation, or not, by where the stack of
            # the thread happens to be. The assembler is asked to use the unaligned moves (they cost the same on an aligned address). The
            # link line gets it too, as link-time optimisation writes the code at the link.
            if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
                check_cxx_compiler_flag("-Wa,-muse-unaligned-vector-move" _have_unaligned_vector_move)
                if(_have_unaligned_vector_move)
                    add_compile_options(-Wa,-muse-unaligned-vector-move)
                    add_link_options(-Wa,-muse-unaligned-vector-move)
                    message(STATUS "Native optimisation: unaligned vector moves (GCC bug 54412 on Windows)")
                endif()
            endif()
        else()
            check_cxx_compiler_flag("-mcpu=native" _have_mcpu_native)
            if(_have_mcpu_native)
                add_compile_options(-mcpu=native)
                message(STATUS "Native optimisation: -mcpu=native")
            endif()
        endif()
    endif()
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Widgets)

add_subdirectory(src/rfx-gui)
