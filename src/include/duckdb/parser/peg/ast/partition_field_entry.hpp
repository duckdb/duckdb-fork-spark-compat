#pragma once

#include "duckdb/parser/column_definition.hpp"
#include "duckdb/parser/parsed_expression.hpp"

namespace duckdb_fork {
using namespace duckdb;
struct PartitionFieldEntry {
	unique_ptr<ParsedExpression> key;     // set for the reference form (PARTITIONED BY (existing_col)), later ignored
	unique_ptr<ColumnDefinition> column;  // set for the typed form (PARTITIONED BY (name type)), appended to schema
};
} // namespace duckdb_fork
