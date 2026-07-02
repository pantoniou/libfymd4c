cmake_minimum_required(VERSION 3.12)

project(libfymd4c
    VERSION "0.0.1"
    HOMEPAGE_URL "https://github.com/pantoniou/libfymd4c"
    LANGUAGES C
)

option(BUILD_FYMD4C_EXECUTABLE "Whether to compile the fymd4c executable" ON)
option(MD4C_WITH_FYTS "Tree-sitter syntax highlighting of fenced code via libfyts" ON)
option(ENABLE_ASAN "Enable ASAN support" OFF)


if(WIN32)
    # On Windows, given there is no standard lib install dir etc., we rather
    # by default build static lib.
    option(BUILD_SHARED_LIBS "Build using shared libraries" OFF)
else()
    # On Linux, MD4C is slowly being adding into some distros which prefer
    # shared lib.
    option(BUILD_SHARED_LIBS "Build using shared libraries" ON)
endif()

set(CMAKE_CONFIGURATION_TYPES Debug Release RelWithDebInfo MinSizeRel)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE $ENV{CMAKE_BUILD_TYPE})

    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE "Release")
    endif()
endif()


if(CMAKE_C_COMPILER_ID MATCHES GNU|Clang)
    add_compile_options(-Wall -Wextra -Wsign-compare -Wshadow)

    # Build against the newest C standard the compiler supports, mirroring
    # libfyaml: gnu2x -> c2x -> gnu11.
    include(CheckCCompilerFlag)
    set(_md4c_std "")
    check_c_compiler_flag(-std=gnu2x MD4C_HAS_GNU2X)
    if(MD4C_HAS_GNU2X)
        set(_md4c_std -std=gnu2x)
    else()
        check_c_compiler_flag(-std=c2x MD4C_HAS_C2X)
        if(MD4C_HAS_C2X)
            set(_md4c_std -std=c2x)
        else()
            check_c_compiler_flag(-std=gnu11 MD4C_HAS_GNU11)
            if(MD4C_HAS_GNU11)
                set(_md4c_std -std=gnu11)
            endif()
        endif()
    endif()
    if(_md4c_std)
        add_compile_options(${_md4c_std})
    endif()

    add_compile_definitions(_GNU_SOURCE)
elseif(MSVC)
    # Disable warnings about the so-called unsecured functions:
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    add_compile_options(/W3)

    # Specify proper C runtime library:
    string(REGEX REPLACE "/M[DT]d?" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
    string(REGEX REPLACE "/M[DT]d?" "" CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")
    string(REGEX REPLACE "/M[DT]d?" "" CMAKE_C_FLAGS_RELWITHDEBINFO "{$CMAKE_C_FLAGS_RELWITHDEBINFO}")
    string(REGEX REPLACE "/M[DT]d?" "" CMAKE_C_FLAGS_MINSIZEREL "${CMAKE_C_FLAGS_MINSIZEREL}")
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} /MTd")
    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /MT")
    set(CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELEASE} /MT")
    set(CMAKE_C_FLAGS_MINSIZEREL "${CMAKE_C_FLAGS_RELEASE} /MT")
endif()

# ASAN support (address + UB + signed-overflow sanitizers), as in libfyaml.
# The flags are applied to libfymd4c (compile + link) and propagated to the
# fymd4c executable through the target's INTERFACE link options.
set(HAVE_ASAN 0)
set(ASAN_C_FLAGS)
if(ENABLE_ASAN)
    set(ASAN_C_FLAGS "-fsanitize=address,signed-integer-overflow,undefined"
                     "-fno-omit-frame-pointer")
    include(CheckCSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS "${ASAN_C_FLAGS}")
    string(REPLACE ";" " " CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS}")
    check_c_source_compiles("int main(void) { return 0; }" ASAN_WORKS)
    unset(CMAKE_REQUIRED_FLAGS)
    if(ASAN_WORKS)
        set(HAVE_ASAN 1)
    else()
        message(FATAL_ERROR "ENABLE_ASAN is on but the sanitizer flags do not work")
    endif()
endif()

include(GNUInstallDirs)

# Shared-library version (soname) for libfymd4c, derived from .libtool-version
# (current:revision:age) exactly as libfyaml does.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/.libtool-version" _LIBTOOL_VERSION)
string(REPLACE ":" ";" _LT_LIST "${_LIBTOOL_VERSION}")
list(GET _LT_LIST 0 LIBTOOL_CURRENT)
list(GET _LT_LIST 1 LIBTOOL_REVISION)
list(GET _LT_LIST 2 LIBTOOL_AGE)
math(EXPR FYMD4C_SOVERSION "${LIBTOOL_CURRENT} - ${LIBTOOL_AGE}")
set(FYMD4C_VERSION_FULL "${FYMD4C_SOVERSION}.${LIBTOOL_AGE}.${LIBTOOL_REVISION}")

if(MD4C_WITH_FYTS)
    # Tree-sitter syntax highlighter (libfyts). We always link its *static*
    # target (`fyts_static` in-tree, `libfyts::fyts_static` when installed) so
    # tree-sitter and the grammars are absorbed (with hidden visibility) into the
    # single libfymd4c shared object, keeping libfyaml its only NEEDED dependency.
    #
    # Provider knob: prefer an installed libfyts, else build it from source.
    #   auto   - use the installed libfyts CMake package if found, else fetch
    #   system - require the installed package (fatal if absent)
    #   fetch  - always build from source via FetchContent
    set(MD4C_FYTS_PROVIDER "auto" CACHE STRING
        "Where to get libfyts: auto, system, or fetch")
    set_property(CACHE MD4C_FYTS_PROVIDER PROPERTY STRINGS auto system fetch)
    if(NOT MD4C_FYTS_PROVIDER MATCHES "^(auto|system|fetch)$")
        message(FATAL_ERROR "MD4C_FYTS_PROVIDER must be one of: auto, system, fetch")
    endif()

    set(_md4c_fyts_from_system 0)
    if(MD4C_FYTS_PROVIDER STREQUAL "system")
        find_package(libfyts CONFIG REQUIRED)
        set(_md4c_fyts_from_system 1)
    elseif(MD4C_FYTS_PROVIDER STREQUAL "auto")
        find_package(libfyts CONFIG QUIET)
        if(libfyts_FOUND)
            set(_md4c_fyts_from_system 1)
        endif()
    endif()

    if(_md4c_fyts_from_system)
        message(STATUS "libfyts: using installed package (${libfyts_DIR})")
        set(MD4C_FYTS_TARGET libfyts::fyts_static)
    else()
        message(STATUS "libfyts: building from source (FetchContent)")
        include(FetchContent)

        # Knob: which language catalogue libfyts fetches and compiles (fetch mode
        # only; a system libfyts ships whatever catalogue it was built with).
        #   minimal - c, cpp, bash, python, diff, json, yaml (smoke set)
        #   default - upstream tree-sitter/* grammars plus diff
        #   full    - the full tree-sitter-language-pack set
        set(MD4C_FYTS_CATALOGUE "minimal" CACHE STRING
            "libfyts language catalogue set: minimal, default, or full")
        set_property(CACHE MD4C_FYTS_CATALOGUE PROPERTY STRINGS minimal default full)
        if(NOT MD4C_FYTS_CATALOGUE MATCHES "^(minimal|default|full)$")
            message(FATAL_ERROR "MD4C_FYTS_CATALOGUE must be one of: minimal, default, full")
        endif()
        # Forward to libfyts (its own cache var; set before MakeAvailable so it wins).
        set(TS_LANGUAGE_CATALOGUE_SET "${MD4C_FYTS_CATALOGUE}" CACHE STRING
            "libfyts language catalogue set" FORCE)

        FetchContent_Declare(libfyts
            GIT_REPOSITORY https://github.com/pantoniou/libfyts.git
            GIT_TAG d0d9cc04972bef3488a48168f879fde99afd30a3
        )
        # Grammars/tree-sitter must be PIC so the static fyts_static links cleanly
        # into our shared libfymd4c.
        set(CMAKE_POSITION_INDEPENDENT_CODE ON)
        FetchContent_MakeAvailable(libfyts)
        set(MD4C_FYTS_TARGET fyts_static)
    endif()
endif()

add_subdirectory(src)
if (BUILD_FYMD4C_EXECUTABLE)
    add_subdirectory(fymd4c)

    # Streaming test: streamed/progressive ANSI output must reconstruct the
    # one-shot render byte-for-byte.
    find_program(PYTHON3_EXECUTABLE NAMES python3 python)
    if (PYTHON3_EXECUTABLE)
        enable_testing()
        add_test(
            NAME stream
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/test"
            COMMAND ${PYTHON3_EXECUTABLE} stream-test.py -p "$<TARGET_FILE:fymd4c-cli>")

        # Golden-output test: block structure (nested lists, blockquote bars).
        add_test(
            NAME render
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/test"
            COMMAND ${PYTHON3_EXECUTABLE} render-test.py -p "$<TARGET_FILE:fymd4c-cli>")

        # CommonMark (+ extension) conformance suites. Each spec file holds a
        # sequence of numbered examples; register every example as its own ctest
        # (driven through `fymd4c -t html`) so `ctest -j` runs them in parallel,
        # mirroring how libfyaml registers each test-suite subtest individually.
        # The example count per file is discovered at configure time from the
        # `<32 backticks> example` markers; run-testsuite.py's `-n N` selects one.
        set(_spec_dir "${CMAKE_CURRENT_SOURCE_DIR}/test")
        file(GLOB _spec_files RELATIVE "${_spec_dir}" "${_spec_dir}/*.txt")
        list(SORT _spec_files)
        foreach(_spec ${_spec_files})
            # Re-run configure if a spec file gains/loses examples.
            set_property(DIRECTORY APPEND PROPERTY
                CMAKE_CONFIGURE_DEPENDS "${_spec_dir}/${_spec}")
            # CMake's regex has no bounded {32} quantifier; `+` matches the run.
            file(STRINGS "${_spec_dir}/${_spec}" _ex_lines REGEX "^`+ example")
            list(LENGTH _ex_lines _ex_count)
            if(_ex_count GREATER 0)
                foreach(_n RANGE 1 ${_ex_count})
                    add_test(
                        NAME "conformance/${_spec}/${_n}"
                        WORKING_DIRECTORY "${_spec_dir}"
                        COMMAND ${PYTHON3_EXECUTABLE} run-testsuite.py
                                -s "${_spec}" -n ${_n}
                                -p "$<TARGET_FILE:fymd4c-cli> -t html")
                    set_tests_properties("conformance/${_spec}/${_n}"
                        PROPERTIES LABELS "conformance")
                endforeach()
            endif()
        endforeach()
    endif ()
endif ()
