cmake_minimum_required(VERSION 3.18 FATAL_ERROR)
project(httplib VERSION 1.0.5)

get_directory_property(_HAS_PARENT PARENT_DIRECTORY)
if(_HAS_PARENT)
    set(IS_ROOT_PROJECT OFF)
else()
    set(IS_ROOT_PROJECT ON)
endif()

if(IS_ROOT_PROJECT)

    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(OUTPUT_DIR_DEBUG ${CMAKE_CURRENT_SOURCE_DIR}/bin/x64/Debug)
        set(OUTPUT_DIR_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/bin/x64/Release)
    else () 
        set(OUTPUT_DIR_DEBUG ${CMAKE_CURRENT_SOURCE_DIR}/bin/x86/Debug)
        set(OUTPUT_DIR_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/bin/x86/Release)
    endif()

    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_DEBUG ${OUTPUT_DIR_DEBUG})
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_DEBUG ${OUTPUT_DIR_DEBUG})
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${OUTPUT_DIR_DEBUG})

    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELEASE ${OUTPUT_DIR_RELEASE})
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_RELEASE ${OUTPUT_DIR_RELEASE})
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${OUTPUT_DIR_RELEASE})

    set_property(GLOBAL PROPERTY USE_FOLDERS ON)

endif(IS_ROOT_PROJECT)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(HTTPLIB_ENABLED_UNITY_BUILD "Enable unity/jumbo builds (CI recommended, slower incremental)" OFF)
if(HTTPLIB_ENABLED_UNITY_BUILD)
    set(CMAKE_UNITY_BUILD ON)
    set(CMAKE_UNITY_BUILD_BATCH_SIZE 8)
endif()

if(MSVC)
    add_compile_options(/MP)
endif()

# Sanitizers. MSVC supports only ASan (x64 only). Clang/GCC support address+undefined.
# TSan is mutually exclusive with ASan and must be requested on its own.
#
# NOTE: output directories are shared with normal builds, so an instrumented httplib.lib
# overwrites the non-instrumented one. Clean bin/ when switching between the two.
set(HTTPLIB_SANITIZER "none" CACHE STRING "Enable sanitizers: none, address, undefined, thread")
set_property(CACHE HTTPLIB_SANITIZER PROPERTY STRINGS none address undefined thread)

if(NOT HTTPLIB_SANITIZER STREQUAL "none")
    message(STATUS "HTTPLIB: sanitizer '${HTTPLIB_SANITIZER}' enabled")

    if(HTTPLIB_SANITIZER STREQUAL "thread")
        if(MSVC)
            message(FATAL_ERROR "HTTPLIB: MSVC has no TSan. Use clang with -fsanitize=thread, or -DCMAKE_CXX_COMPILER=clang-cl.")
        endif()
        add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=thread)
    elseif(HTTPLIB_SANITIZER STREQUAL "address" OR HTTPLIB_SANITIZER STREQUAL "undefined")
        if(MSVC)
            if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
                message(FATAL_ERROR "HTTPLIB: MSVC ASan requires a 64-bit (x64) target.")
            endif()
            if(HTTPLIB_SANITIZER STREQUAL "undefined")
                message(FATAL_ERROR "HTTPLIB: MSVC has no UBSan. Use clang with -fsanitize=undefined.")
            endif()
            add_compile_options(/fsanitize=address)
            add_link_options(/fsanitize=address)
            # ASan needs the debug CRT to interpose correctly, and optimization off keeps
            # stack traces usable.
            add_compile_options(/Zi /Od)
        else()
            add_compile_options(-fsanitize=${HTTPLIB_SANITIZER} -fno-omit-frame-pointer -g)
            add_link_options(-fsanitize=${HTTPLIB_SANITIZER})
            add_compile_options(-fno-optimize-sibling-calls)
        endif()
    else()
        message(FATAL_ERROR "HTTPLIB: unknown HTTPLIB_SANITIZER '${HTTPLIB_SANITIZER}'. Use none, address, undefined or thread.")
    endif()

    # A unity TU merges unrelated TUs into one, which hides sanitizer reports.
    if(HTTPLIB_ENABLED_UNITY_BUILD)
        message(WARNING "HTTPLIB: HTTPLIB_ENABLED_UNITY_BUILD is forced OFF under sanitizers "
                        "(a unity TU merges unrelated TUs, which hides sanitizer reports).")
    endif()
    set(CMAKE_UNITY_BUILD OFF)
endif()

set(HTTPLIB_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)
set(HTTPLIB_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lib)


option(HTTPLIB_ENABLED_SSL "HTTLIB ENABLED SSL" OFF)
option(HTTPLIB_ENABLED_COMPRESS "HTTLIB ENABLED COMPRESS" OFF)
option(HTTPLIB_ENABLED_DATABASE "HTTLIB ENABLED DATABASE (requires Boost.MySQL 1.85+)" OFF)
option(HTTPLIB_ENABLED_EXAMPLES "HTTLIB Build Examples" ${IS_ROOT_PROJECT})
option(HTTPLIB_ENABLED_TESTS "HTTLIB Build Tests" ${IS_ROOT_PROJECT})
option(HTTPLIB_SHARED_LIBRARY "HTTLIB Build Shared Library" OFF)

add_subdirectory(lib)

if(HTTPLIB_ENABLED_EXAMPLES)
    add_subdirectory(examples)
endif()

if(HTTPLIB_ENABLED_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
