#pragma once
#include "duckdb/parser/parsed_expression.hpp"

namespace duckdb_fork {
using namespace duckdb;
struct DistinctClause {
	bool is_distinct;
	vector<unique_ptr<ParsedExpression>> distinct_targets;
};
} // namespace duckdb_fork
