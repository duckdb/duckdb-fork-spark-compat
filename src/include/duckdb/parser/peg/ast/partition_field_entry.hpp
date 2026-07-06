#pragma once

#include "duckdb/common/optional.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/parser/parsed_expression.hpp"

namespace duckdb_fork {
using namespace duckdb;
// Mirrors the PartitionField <- Expression Type? grammar rule. A bare expression is a reference to an
// existing column (later ignored); with a type it declares a new typed partition column appended to the schema.
struct PartitionFieldEntry {
	unique_ptr<ParsedExpression> expression;
	optional<LogicalType> type;
};
} // namespace duckdb_fork
