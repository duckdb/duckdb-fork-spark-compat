#pragma once
#include "duckdb/common/common.hpp"

#include "duckdb/common/identifier.hpp"
namespace duckdb_fork {
using namespace duckdb;
struct AnalyzeTarget {
	unique_ptr<TableRef> ref;
	vector<Identifier> columns;
};
} // namespace duckdb_fork
