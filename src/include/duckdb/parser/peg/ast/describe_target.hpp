#pragma once

#include "duckdb/common/identifier.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"

namespace duckdb_fork {
using namespace duckdb;

struct DescribeTarget {
	bool is_table_name = false;
	Identifier table_name;
	unique_ptr<BaseTableRef> table_ref;
};

} // namespace duckdb_fork
