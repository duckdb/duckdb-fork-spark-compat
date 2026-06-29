#pragma once

#include "duckdb/common/identifier.hpp"
#include "duckdb/parser/parsed_expression.hpp"

namespace duckdb_fork {
using namespace duckdb;
// One item of a Spark static/dynamic partition spec: `PARTITION (col [= value], ...)`.
struct PartitionSpecEntry {
	Identifier name;                  // partition column name
	unique_ptr<ParsedExpression> value;  // static value (col = value); null for a dynamic column (bare col)
};
} // namespace duckdb_fork
