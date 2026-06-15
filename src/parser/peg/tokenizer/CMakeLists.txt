add_library_unity(duckdb_fork_parser_peg_tokenizer OBJECT base_tokenizer.cpp
                  highlight_tokenizer.cpp parser_tokenizer.cpp)

set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_fork_parser_peg_tokenizer>
    PARENT_SCOPE)
