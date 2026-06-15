#pragma once

#include "duckdb/parser/peg/ast/generated_column_definition.hpp"

namespace duckdb_fork {
using namespace duckdb;
struct CreateTableColumnElement {
	unique_ptr<ConstraintColumnDefinition> column_definition;
	unique_ptr<Constraint> constraint;
};
} // namespace duckdb_fork
