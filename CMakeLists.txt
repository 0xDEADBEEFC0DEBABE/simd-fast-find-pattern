cmake_minimum_required(VERSION 3.21)
project(fastpattern LANGUAGES C)

include(GNUInstallDirs)

option(FASTPATTERN_BUILD_TESTS "Build correctness and concurrency tests" ON)
option(FASTPATTERN_BUILD_BENCHMARKS "Build the benchmark executable" OFF)
option(FASTPATTERN_SANITIZE "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(FASTPATTERN_DISABLE_SIMD "Use only the portable scalar implementation" OFF)

function(fp_configure_target target)
    set_target_properties(${target} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    if(FASTPATTERN_SANITIZE)
        if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND NOT MSVC)
            target_compile_options(${target} PRIVATE
                -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(${target} PUBLIC -fsanitize=address,undefined)
        else()
            message(FATAL_ERROR "FASTPATTERN_SANITIZE requires GCC or Clang")
        endif()
    endif()
endfunction()

add_library(fastpattern fastpattern.c)
add_library(fastpattern::fastpattern ALIAS fastpattern)
target_include_directories(fastpattern PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
if(FASTPATTERN_DISABLE_SIMD)
    target_compile_definitions(fastpattern PRIVATE FP_DISABLE_SIMD=1)
endif()
if(WIN32 AND BUILD_SHARED_LIBS)
    set_target_properties(fastpattern PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()
fp_configure_target(fastpattern)

add_executable(fp_example example_simple.c)
target_link_libraries(fp_example PRIVATE fastpattern)
fp_configure_target(fp_example)

if(FASTPATTERN_BUILD_BENCHMARKS)
    add_executable(fp_benchmark benchmark.c)
    target_link_libraries(fp_benchmark PRIVATE fastpattern)
    fp_configure_target(fp_benchmark)
endif()

if(FASTPATTERN_BUILD_TESTS)
    enable_testing()
    enable_language(CXX)
    find_package(Threads REQUIRED)

    function(fp_add_test_suite suffix library)
        add_executable(fp_test_${suffix} tests/test_fastpattern.c)
        target_link_libraries(fp_test_${suffix} PRIVATE ${library})
        fp_configure_target(fp_test_${suffix})
        add_test(NAME correctness_${suffix} COMMAND fp_test_${suffix})

        add_executable(fp_threads_${suffix} tests/test_threads.c)
        target_link_libraries(fp_threads_${suffix} PRIVATE ${library} Threads::Threads)
        fp_configure_target(fp_threads_${suffix})
        add_test(NAME threads_${suffix} COMMAND fp_threads_${suffix})
        add_test(NAME shared_compiled_${suffix} COMMAND fp_threads_${suffix} --shared-compiled)
        set_tests_properties(correctness_${suffix} threads_${suffix} shared_compiled_${suffix}
            PROPERTIES TIMEOUT 120)
    endfunction()

    fp_add_test_suite(default fastpattern)

    # Separate binaries exercise each compiled dispatch ceiling without requiring
    # AVX support on the host: runtime CPU checks still apply inside the library.
    set(fp_test_variants scalar)
    if(NOT FASTPATTERN_DISABLE_SIMD AND
       (CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|i[3-6]86|x86)$" OR
        "x86_64" IN_LIST CMAKE_OSX_ARCHITECTURES))
        list(APPEND fp_test_variants sse2 avx2)
    endif()
    foreach(variant IN LISTS fp_test_variants)
        add_library(fp_backend_${variant} STATIC fastpattern.c)
        target_include_directories(fp_backend_${variant} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
        if(variant STREQUAL "scalar")
            target_compile_definitions(fp_backend_${variant} PRIVATE FP_DISABLE_SIMD=1)
        elseif(variant STREQUAL "sse2")
            target_compile_definitions(fp_backend_${variant} PRIVATE FP_MAX_SIMD_BITS=128)
        elseif(variant STREQUAL "avx2")
            target_compile_definitions(fp_backend_${variant} PRIVATE FP_MAX_SIMD_BITS=256)
        endif()
        fp_configure_target(fp_backend_${variant})
        fp_add_test_suite(${variant} fp_backend_${variant})
    endforeach()

    add_executable(fp_cpp_smoke tests/test_cpp.cpp)
    set_target_properties(fp_cpp_smoke PROPERTIES
        CXX_STANDARD 11 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    target_link_libraries(fp_cpp_smoke PRIVATE fastpattern)
    fp_configure_target(fp_cpp_smoke)
    add_test(NAME cpp_header COMMAND fp_cpp_smoke)
endif()

install(TARGETS fastpattern EXPORT fastpatternTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES fastpattern.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/fastpattern)
install(EXPORT fastpatternTargets FILE fastpatternConfig.cmake
    NAMESPACE fastpattern:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fastpattern)
