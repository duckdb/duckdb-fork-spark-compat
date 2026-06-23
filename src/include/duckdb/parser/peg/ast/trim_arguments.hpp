#pragma once

#include "duckdb/common/optional.hpp"
#include "duckdb/parser/parsed_expression.hpp"

namespace duckdb_fork {
using namespace duckdb;

struct TrimArguments {
	optional<string> trim_direction;
	vector<unique_ptr<ParsedExpression>> expressions;
};

} // namespace duckdb_fork
