# Build rules for the fymd4c command line utility: the single Markdown
# converter over libfymd4c. Renders to ANSI (default), HTML, or heals
# mid-stream Markdown. Links only libfymd4c (self-contained; libfyaml is its
# sole runtime dependency).

# The library target is named `fymd4c`; use a distinct target name for the
# executable but keep its output binary named `fymd4c`.
add_executable(fymd4c-cli fymd4c.c)
set_target_properties(fymd4c-cli PROPERTIES OUTPUT_NAME fymd4c)
target_link_libraries(fymd4c-cli PRIVATE fymd4c)
target_compile_definitions(fymd4c-cli PRIVATE
    MD_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    MD_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    MD_VERSION_RELEASE=${PROJECT_VERSION_PATCH}
)


# Install rules

install(
    TARGETS fymd4c-cli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES "fymd4c.1" DESTINATION "${CMAKE_INSTALL_MANDIR}/man1")
