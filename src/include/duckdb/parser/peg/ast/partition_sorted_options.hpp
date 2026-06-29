#pragma once
#include "duckdb/common/vector.hpp"
#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/parser/column_list.hpp"

namespace duckdb_fork {
using namespace duckdb;
struct PartitionSortedOptions {
	vector<unique_ptr<ParsedExpression>> partition_keys;
	vector<unique_ptr<ParsedExpression>> sort_keys;
	// typed partition columns (PARTITIONED BY (name type)) appended to the table schema
	ColumnList partition_columns;
};
} // namespace duckdb_fork
