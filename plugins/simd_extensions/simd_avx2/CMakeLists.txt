cmake_minimum_required(VERSION 3.12)
project(simd_avx2)

if(NOT ANDROID)
    include(FindAVX)
endif()

if(ANDROID)
    set(CXX_AVX2_FOUND 0)

    if(ANDROID_ABI STREQUAL "x86")
        set(CXX_AVX2_FOUND 1)
    endif()

    if(ANDROID_ABI STREQUAL "x86_64")
        set(CXX_AVX2_FOUND 1)
    endif()
endif()

# CXX_AVX2_FOUND is FindAVX's overall result: the C++ compiler builds AVX2 code, with or
# without the flags it found (CXX_AVX2_FLAGS). Its per-attempt results (CXX_HAS_<type>_<n>)
# depend on which attempt succeeded, so they are not used here.
# MSVC compiles the AVX2 test program without any flag, so CXX_AVX2_FOUND is TRUE there and
# this would build the plugin on Windows x64 for the first time. It has never been compiled
# with MSVC: keep it off there (MSVC also covers clang-cl) unless PLUGIN_SIMD_AVX2_MSVC is
# set, until a Windows build has been checked.
if(CXX_AVX2_FOUND AND (NOT MSVC OR PLUGIN_SIMD_AVX2_MSVC))
    message("Found AVX2 features, enabling AVX2 plugin")

    file(GLOB_RECURSE simd_avx2_CPPS *.cpp)
    add_library(simd_avx2 SHARED ${simd_avx2_CPPS})
    target_link_libraries(simd_avx2 PUBLIC satdump_core)
    target_include_directories(simd_avx2 PUBLIC src)

    if(UNIX OR ANDROID)
        set(FINAL_FLAGS_AVX2 "-mavx2")
    elseif(MSVC)
        # Not CXX_AVX2_FLAGS: FindAVX's first attempt (no flag) succeeds on MSVC, so that is
        # " " there. clang-cl needs /arch:AVX2 for the intrinsics; MSVC accepts them without
        # it, but then mixes legacy SSE code with them (SSE/AVX transition penalties).
        set(FINAL_FLAGS_AVX2 "/arch:AVX2")
    endif()

    # Enable on all files but main.cpp
    foreach(_file ${simd_avx2_CPPS})
        if(NOT(${_file} MATCHES "main.cpp$"))
            set_source_files_properties(${_file} PROPERTIES COMPILE_FLAGS "${FINAL_FLAGS_AVX2}")
        endif()
    endforeach()

    install(TARGETS simd_avx2 DESTINATION ${CMAKE_INSTALL_LIBDIR}/satdump/plugins)
else()
    message("AVX2 Features not found! This is only an error on x86 CPUs that should support AVX2")
endif()