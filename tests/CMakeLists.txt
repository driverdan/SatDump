cmake_minimum_required(VERSION 3.16)

# Use a system Catch2 if it is recent enough, otherwise fetch a current one
set(SATDUMP_CATCH2_MIN_VERSION 3.4.0)
set(SATDUMP_CATCH2_FETCH_VERSION 3.16.1)
find_package(Catch2 ${SATDUMP_CATCH2_MIN_VERSION} QUIET)

if(NOT Catch2_FOUND)
    message("Catch2 >= ${SATDUMP_CATCH2_MIN_VERSION} not found, fetching ${SATDUMP_CATCH2_FETCH_VERSION}")
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v${SATDUMP_CATCH2_FETCH_VERSION}
        GIT_SHALLOW ON
    )
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
endif()

# Tests mirror the source tree: the tests for <root>/<path> are in
# tests/<root>/<path>. Everything else under tests/ is shared by all of them.
set(SATDUMP_TESTED_ROOTS src-core src-cli plugins)

file(GLOB_RECURSE SatDump_tests_CPPS CONFIGURE_DEPENDS *.cpp)

# The tests of a plugin are only built with that plugin, see below
file(GLOB_RECURSE SatDump_tests_plugins_CPPS CONFIGURE_DEPENDS plugins/*.cpp)
if(SatDump_tests_plugins_CPPS)
    list(REMOVE_ITEM SatDump_tests_CPPS ${SatDump_tests_plugins_CPPS})
endif()

# The CLI is an executable, so its sources are compiled into the tests to be
# reachable from them. Its entry point (src-cli/main.cpp) is left out.
file(GLOB_RECURSE SatDump_tests_cli_CPPS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src-cli/*.cpp)
list(REMOVE_ITEM SatDump_tests_cli_CPPS ${CMAKE_SOURCE_DIR}/src-cli/main.cpp)

add_executable(satdump_tests ${SatDump_tests_CPPS} ${SatDump_tests_cli_CPPS})
target_link_libraries(satdump_tests PRIVATE satdump_core Catch2::Catch2)

# Shared test helpers are included relative to tests/ (e.g. "helpers/temp_files.h")
target_include_directories(satdump_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Test helpers of a source root are included relative to tests/<root>/, like
# its headers are relative to <root>/ (e.g. "dsp/ndsp_test_helpers.h")
foreach(root ${SATDUMP_TESTED_ROOTS})
    target_include_directories(satdump_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/${root})
endforeach()

# src-cli headers are included relative to src-cli/ (e.g. "legacy/webserver.h")
target_include_directories(satdump_tests PRIVATE ${CMAKE_SOURCE_DIR}/src-cli)

# Plugin headers are included relative to plugins/ (e.g. "xrit_support/xrit/xrit_file.h")
target_include_directories(satdump_tests PRIVATE ${CMAKE_SOURCE_DIR}/plugins)

# Adds the tests in tests/plugins/<dir> if the plugin is built, and links the
# plugin into the tests. SATDUMP_TEST_PLUGIN_<TARGET> is then the path of the
# plugin, to load it the way satdump does. Plugins export nothing with MSVC.
set(SATDUMP_TESTED_PLUGINS)
function(satdump_add_plugin_tests target dir)
    if(MSVC OR NOT TARGET ${target})
        message("Not building the unit tests of ${target}")
        return()
    endif()

    file(GLOB_RECURSE plugin_tests_CPPS CONFIGURE_DEPENDS plugins/${dir}/*.cpp)
    target_sources(satdump_tests PRIVATE ${plugin_tests_CPPS})
    target_link_libraries(satdump_tests PRIVATE ${target})
    string(TOUPPER ${target} target_upper)
    target_compile_definitions(satdump_tests PRIVATE SATDUMP_TEST_PLUGIN_${target_upper}="$<TARGET_FILE:${target}>")

    list(APPEND SATDUMP_TESTED_PLUGINS ${target})
    set(SATDUMP_TESTED_PLUGINS ${SATDUMP_TESTED_PLUGINS} PARENT_SCOPE)
    set(SATDUMP_TESTED_PLUGIN_DIR_${target} ${dir} PARENT_SCOPE)
endfunction()

satdump_add_plugin_tests(rtltcp_support sdr_sources/rtltcp_support)
satdump_add_plugin_tests(remote_sdr_support sdr_sources/remote_sdr_support)
satdump_add_plugin_tests(net_source_support sdr_sources/net_source_support)
satdump_add_plugin_tests(xrit_support xrit_support)
satdump_add_plugin_tests(goes_support goes_support)
satdump_add_plugin_tests(fengyun2_support fengyun2_support)
satdump_add_plugin_tests(gk2a_support gk2a_support)
satdump_add_plugin_tests(fengyun4_support fengyun4_support)
satdump_add_plugin_tests(elektro_arktika_support elektro_arktika_support)
satdump_add_plugin_tests(jpss_support jpss_support)
satdump_add_plugin_tests(meteor_support meteor_support)
satdump_add_plugin_tests(fengyun3_support fengyun3_support)
satdump_add_plugin_tests(noaa_metop_support noaa_metop_support)
satdump_add_plugin_tests(dvb_support dvb_support)
satdump_add_plugin_tests(webhook_app webhook_app)

# Lets tests load files shipped in the repository (e.g. resources/)
target_compile_definitions(satdump_tests PRIVATE SATDUMP_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

# Register every TEST_CASE with CTest. The timeout is well above the longest
# bounded wait in the test helpers, so a hung test fails instead of stalling
# the run for CTest's 25 minutes default.
#
# Catch2 exits with 4 when every test it ran was skipped. Older versions of
# catch_discover_tests do not tell CTest about it, which then reports a
# SKIP() as a failure.
include(Catch)
catch_discover_tests(satdump_tests PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 4)

# Code coverage
if(ENABLE_COVERAGE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "ENABLE_COVERAGE is only supported with GCC and Clang")
    endif()

    message("Building with coverage instrumentation")

    # -O0 is added last so it overrides the build type's optimization level,
    # otherwise inlining and elided code skew the line counts.
    #
    # The counters are updated atomically where the compiler supports it : the
    # DSP blocks run in their own threads, and racing plain increments leave
    # inconsistent counts that gcov reports as negative hits, which gcovr
    # rejects.
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag(-fprofile-update=atomic COVERAGE_HAS_ATOMIC_PROFILE_UPDATE)
    set(COVERAGE_COMPILE_OPTIONS --coverage -O0 -g)
    if(COVERAGE_HAS_ATOMIC_PROFILE_UPDATE)
        list(APPEND COVERAGE_COMPILE_OPTIONS -fprofile-update=atomic)
    endif()

    set(COVERAGE_PLUGIN_FILTERS)
    foreach(target satdump_core satdump_tests ${SATDUMP_TESTED_PLUGINS})
        target_compile_options(${target} PRIVATE ${COVERAGE_COMPILE_OPTIONS})
        target_link_options(${target} PRIVATE --coverage)
        if(SATDUMP_TESTED_PLUGIN_DIR_${target})
            list(APPEND COVERAGE_PLUGIN_FILTERS --filter ${CMAKE_SOURCE_DIR}/plugins/${SATDUMP_TESTED_PLUGIN_DIR_${target}}/)
        endif()
    endforeach()

    # "coverage" target : run the tests, then generate a report with gcovr
    find_program(GCOVR_EXECUTABLE gcovr)

    if(GCOVR_EXECUTABLE)
        set(COVERAGE_GCOV_ARGS)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            find_program(LLVM_COV_EXECUTABLE llvm-cov REQUIRED)
            set(COVERAGE_GCOV_ARGS --gcov-executable "${LLVM_COV_EXECUTABLE} gcov")
        endif()

        set(COVERAGE_DIR ${CMAKE_BINARY_DIR}/coverage)

        # Unoptimized instrumented tests are slow, so run them in parallel
        include(ProcessorCount)
        ProcessorCount(COVERAGE_JOBS)
        if(COVERAGE_JOBS EQUAL 0)
            set(COVERAGE_JOBS 1)
        endif()

        add_custom_target(coverage
            COMMAND ${CMAKE_COMMAND} -E make_directory ${COVERAGE_DIR}
            COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -j ${COVERAGE_JOBS}
            COMMAND ${GCOVR_EXECUTABLE}
                --root ${CMAKE_SOURCE_DIR}
                --filter ${CMAKE_SOURCE_DIR}/src-core/
                --filter ${CMAKE_SOURCE_DIR}/src-cli/
                ${COVERAGE_PLUGIN_FILTERS}
                # Vendored third-party code
                --exclude ${CMAKE_SOURCE_DIR}/src-core/libs/
                --exclude ${CMAKE_SOURCE_DIR}/src-core/imgui/
                --exclude ${CMAKE_SOURCE_DIR}/src-core/nlohmann/
                --exclude ${CMAKE_SOURCE_DIR}/src-core/angelscript/
                ${COVERAGE_GCOV_ARGS}
                --delete # Reset the counters, so every run starts from zero
                --print-summary
                --xml ${COVERAGE_DIR}/coverage.xml
                --html-details ${COVERAGE_DIR}/index.html
                -j ${COVERAGE_JOBS}
                ${CMAKE_BINARY_DIR}
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            DEPENDS satdump_tests
            USES_TERMINAL
            COMMENT "Running tests and generating coverage report in ${COVERAGE_DIR}"
        )
    else()
        message(WARNING "gcovr not found, the 'coverage' report target will not be available (pip install gcovr)")
    endif()
endif()
