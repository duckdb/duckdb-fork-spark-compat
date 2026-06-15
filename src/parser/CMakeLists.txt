# The fork contributes only the parsing machinery (namespace duckdb_fork):
# parser.cpp + the PEG subtree. AST classes (statement/, expression/, ...) are
# NOT compiled here - they resolve to the host DuckDB's namespace duckdb symbols.

# Expose ONLY this tree's duckdb/parser headers to the fork's TUs; every other
# header (duckdb/common, duckdb/main, ...) must resolve to the host DuckDB's
# tree so the fork cannot silently compile against diverged shared headers.
# An include dir can only expose a whole root, so build an overlay containing
# a single symlink duckdb/parser -> <fork>/src/include/duckdb/parser.
set(FORK_PARSER_INCLUDE_OVERLAY ${CMAKE_CURRENT_BINARY_DIR}/include_overlay)
file(MAKE_DIRECTORY ${FORK_PARSER_INCLUDE_OVERLAY}/duckdb)
if(NOT EXISTS ${FORK_PARSER_INCLUDE_OVERLAY}/duckdb/parser)
    file(CREATE_LINK ${CMAKE_CURRENT_SOURCE_DIR}/../include/duckdb/parser
            ${FORK_PARSER_INCLUDE_OVERLAY}/duckdb/parser SYMBOLIC)
endif()
include_directories(BEFORE ${FORK_PARSER_INCLUDE_OVERLAY})

# Start from a clean object list: only fork objects may flow to the parent scope.
set(ALL_OBJECT_FILES)

add_subdirectory(peg)

add_library_unity(duckdb_fork_parser OBJECT parser.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_fork_parser>
    PARENT_SCOPE)
