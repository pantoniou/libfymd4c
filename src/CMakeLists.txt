set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS 1)

# Handle absolute include and lib dirs outside of CMAKE_INSTALL_PREFIX
include(JoinPaths.cmake) # can be replaced by cmake_path(APPEND) in CMake 3.20
join_paths(PKGCONFIG_INCLUDEDIR "\${prefix}" "${CMAKE_INSTALL_INCLUDEDIR}")
join_paths(PKGCONFIG_LIBDIR "\${prefix}" "${CMAKE_INSTALL_LIBDIR}")

# libfyaml: the ANSI renderer's styling config (md4c-style) is parsed from YAML,
# and the public header exposes a fy_generic styling option. A parent project
# may already provide libfyaml::libfyaml (its own find_package/FetchContent,
# e.g. when libfymd4c is pulled in transitively via fyai); reuse that instead
# of re-resolving the library, then try libfyaml's own CMake package config,
# and only then fall back to a raw path lookup.
if(NOT TARGET libfyaml::libfyaml)
    find_package(libfyaml CONFIG QUIET)
endif()
if(TARGET libfyaml::libfyaml)
    get_target_property(LIBFYAML_INCLUDE_DIR libfyaml::libfyaml INTERFACE_INCLUDE_DIRECTORIES)
else()
    find_path(LIBFYAML_INCLUDE_DIR NAMES libfyaml.h)
    find_library(LIBFYAML_LIBRARY NAMES fyaml)
    if(NOT LIBFYAML_INCLUDE_DIR OR NOT LIBFYAML_LIBRARY)
        message(FATAL_ERROR "libfyaml not found (required by the ANSI renderer styling)")
    endif()
endif()

# Embed the default styling YAML as a C byte array.
set(_md4c_style_yaml "${CMAKE_CURRENT_SOURCE_DIR}/../stylings/md4c-default.yaml")
set(_md4c_style_inc "${CMAKE_CURRENT_BINARY_DIR}/generated/md4c_default_style.inc")
# Re-run CMake configure (regenerating the .inc below) whenever the YAML changes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_md4c_style_yaml}")
file(READ "${_md4c_style_yaml}" _md4c_style_hex HEX)
string(REGEX REPLACE "(..)" "0x\\1," _md4c_style_bytes "${_md4c_style_hex}")
file(WRITE "${_md4c_style_inc}"
    "/* Generated from stylings/md4c-default.yaml. */\n"
    "static const unsigned char MD4C_DEFAULT_STYLE[] = {${_md4c_style_bytes}};\n"
    "static const size_t MD4C_DEFAULT_STYLE_LEN = sizeof(MD4C_DEFAULT_STYLE);\n")

# The shipped library: libfymd4c. Everything (md4c parser + entity table, HTML
# renderer, heal utility, ANSI renderer, styling and streaming front-end) is
# compiled in and hidden; the only exported symbols are the fymd_* public API.
# There are no standalone md4c / md4c-html / md4c-heal libraries.
#
# Built in two flavours from the same sources, mirroring libfyaml / libfyts:
#   fymd4c        - the primary library (shared when BUILD_SHARED_LIBS). The
#                   shared object absorbs the static libfyts + tree-sitter, so it
#                   is self-contained: its only NEEDED dependency is libfyaml.
#   fymd4c_static - a plain static archive. An archive cannot absorb another
#                   archive, so it declares its dependencies (libfyaml, and
#                   libfyts when enabled) through the CMake package's
#                   find_dependency() -- consumers link those too.
set(_fymd4c_incdir "${CMAKE_CURRENT_SOURCE_DIR}/../include")
configure_file(libfymd4c.pc.in libfymd4c.pc @ONLY)

# Prefer libfyaml's imported CMake targets (clean, relocatable interface); fall
# back to the raw library path if the package is unavailable. Keep the shared
# and static dependencies separate so libfymd4c_static does not pull in the
# shared libfyaml target when libfyaml's static target is installed.
find_package(libfyaml CONFIG QUIET)
if(TARGET libfyaml::libfyaml)
    set(_fymd4c_fyaml_shared libfyaml::libfyaml)
else()
    set(_fymd4c_fyaml_shared "${LIBFYAML_LIBRARY}")
endif()
if(TARGET libfyaml::fyaml_static)
    set(_fymd4c_fyaml_static libfyaml::fyaml_static)
else()
    set(_fymd4c_fyaml_static "${LIBFYAML_LIBRARY}")
endif()
if(NOT BUILD_SHARED_LIBS AND TARGET libfyts::fyts_static AND TARGET libfyaml::fyaml_static)
    get_target_property(_fymd4c_fyts_static_deps libfyts::fyts_static INTERFACE_LINK_LIBRARIES)
    if(_fymd4c_fyts_static_deps)
        list(TRANSFORM _fymd4c_fyts_static_deps REPLACE "^libfyaml::libfyaml$" "libfyaml::fyaml_static")
        list(TRANSFORM _fymd4c_fyts_static_deps REPLACE "^libfyaml::fyaml$" "libfyaml::fyaml_static")
        set_target_properties(libfyts::fyts_static PROPERTIES
            INTERFACE_LINK_LIBRARIES "${_fymd4c_fyts_static_deps}")
    endif()
    unset(_fymd4c_fyts_static_deps)
endif()

set(_fymd4c_sources
    libfymd4c.c
    md4c.c md4c.h
    entity.c entity.h
    md4c-html.c md4c-html.h
    md4c-heal.c md4c-heal.h md4c-heal-wrap.h
    md4c-ansi.c md4c-ansi.h
    md4c-style.c md4c-style.h
    md4c-stream.c md4c-stream.h
)

if(CMAKE_C_COMPILER_ID MATCHES GNU|Clang)
    include(CheckCSourceCompiles)
    set(CMAKE_REQUIRED_LINK_OPTIONS "-Wl,--exclude-libs,ALL")
    check_c_source_compiles("int main(void){return 0;}" FYMD4C_HAVE_EXCLUDE_LIBS)
    unset(CMAKE_REQUIRED_LINK_OPTIONS)
endif()

add_library(fymd4c ${_fymd4c_sources})
add_library(fymd4c_static STATIC ${_fymd4c_sources})

# Keep the static archive named libfymd4c.a next to libfymd4c.so on ELF; use a
# distinct name where they would collide.
set(_fymd4c_static_output fymd4c_static)
if(BUILD_SHARED_LIBS AND NOT WIN32)
    set(_fymd4c_static_output fymd4c)
endif()

# Common configuration for both flavours.
foreach(_t fymd4c fymd4c_static)
    target_include_directories(${_t} PUBLIC
        "$<BUILD_INTERFACE:${_fymd4c_incdir}>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"   # md4c.h companion header
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
        # The public header includes <libfyaml/libfyaml-generic.h>; in-tree
        # consumers need its include dir. Installed consumers get it via the
        # libfyaml package / pkg-config Requires.
        "$<BUILD_INTERFACE:${LIBFYAML_INCLUDE_DIR}>"
    )
    target_include_directories(${_t} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_compile_definitions(${_t} PRIVATE "FYMD_VERSION_STRING=\"${PROJECT_VERSION}\"")
    target_compile_definitions(${_t} PRIVATE "$<$<CONFIG:Debug>:DEBUG>")
    if(CMAKE_C_COMPILER_ID MATCHES GNU|Clang)
        # Hide everything compiled in; only FYMD_EXPORT-tagged fymd_* symbols are
        # public. For the shared object, also hide symbols pulled in from the
        # static libfyts/tree-sitter (a no-op link flag for an archive).
        target_compile_options(${_t} PRIVATE -fvisibility=hidden)
        if(FYMD4C_HAVE_EXCLUDE_LIBS)
            target_link_options(${_t} PRIVATE "-Wl,--exclude-libs,ALL")
        endif()
    endif()
    if(HAVE_ASAN)
        # Instrument the library; propagate the runtime to anything linking it
        # via INTERFACE link options, as libfyaml does.
        target_compile_options(${_t} PRIVATE ${ASAN_C_FLAGS})
        target_link_options(${_t} PRIVATE ${ASAN_C_FLAGS} INTERFACE ${ASAN_C_FLAGS})
    endif()
    if(MD4C_WITH_FYTS)
        target_compile_definitions(${_t} PRIVATE MD4C_WITH_FYTS)
    endif()
endforeach()

# libfyaml is a public dependency of both flavours. The static fymd4c archive
# must prefer libfyaml's static archive when the package provides it.
if(BUILD_SHARED_LIBS)
    target_link_libraries(fymd4c PUBLIC ${_fymd4c_fyaml_shared})
else()
    target_link_libraries(fymd4c PUBLIC ${_fymd4c_fyaml_static})
endif()
target_link_libraries(fymd4c_static PUBLIC ${_fymd4c_fyaml_static})

set_target_properties(fymd4c PROPERTIES
    OUTPUT_NAME fymd4c
    VERSION ${FYMD4C_VERSION_FULL}
    SOVERSION ${FYMD4C_SOVERSION}
    PUBLIC_HEADER "${_fymd4c_incdir}/libfymd4c.h"
    EXPORT_NAME fymd4c
)
set_target_properties(fymd4c_static PROPERTIES
    OUTPUT_NAME ${_fymd4c_static_output}
    POSITION_INDEPENDENT_CODE ON
    EXPORT_NAME fymd4c_static
)

if(MD4C_WITH_FYTS)
    # Shared: absorb the static libfyts (${MD4C_FYTS_TARGET}); it is hidden and
    # must not appear in the installed interface -> build-interface only.
    target_link_libraries(fymd4c PRIVATE $<BUILD_INTERFACE:${MD4C_FYTS_TARGET}>)
    # Static: an archive can't absorb it. When libfyts is the installed package,
    # propagate libfyts::fyts_static as a real dependency (the package config
    # adds find_dependency(libfyts)); with an in-tree fetched libfyts (never
    # installed) it can only be a build-interface dependency.
    if(_md4c_fyts_from_system)
        target_link_libraries(fymd4c_static PUBLIC ${MD4C_FYTS_TARGET})
    else()
        target_link_libraries(fymd4c_static PRIVATE $<BUILD_INTERFACE:${MD4C_FYTS_TARGET}>)
    endif()
endif()

add_library(libfymd4c::libfymd4c        ALIAS fymd4c)
add_library(libfymd4c::libfymd4c_static ALIAS fymd4c_static)

# Whether the installed static archive needs find_dependency(libfyts) (fed to
# the package config template).
if(MD4C_WITH_FYTS AND _md4c_fyts_from_system)
    set(FYMD4C_STATIC_NEEDS_FYTS 1)
else()
    set(FYMD4C_STATIC_NEEDS_FYTS 0)
endif()


# Install rules (libfymd4c only; its own export set / package, libfyaml-style).
install(
    TARGETS fymd4c fymd4c_static
    EXPORT libfymd4c-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(FILES
    "${_fymd4c_incdir}/libfymd4c/libfymd4c-util.h"
    "${_fymd4c_incdir}/libfymd4c/libfymd4c-renderer.h"
    "${_fymd4c_incdir}/libfymd4c/libfymd4c-convert.h"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/libfymd4c)
# md4c.h is installed as a companion: the public API documents cfg.parser_flags
# as md4c MD_FLAG_* and HTML parser_flags likewise, so consumers need the flag
# constants and MD_CHAR/MD_SIZE typedefs.
install(FILES md4c.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/libfymd4c.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

install(EXPORT libfymd4c-targets
    FILE libfymd4c-targets.cmake
    NAMESPACE libfymd4c::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libfymd4c)

include(CMakePackageConfigHelpers)
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/libfymd4c-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/libfymd4c-config.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libfymd4c)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/libfymd4c-config-version.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY AnyNewerVersion)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/libfymd4c-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/libfymd4c-config-version.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libfymd4c)
