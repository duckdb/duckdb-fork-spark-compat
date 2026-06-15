#pragma once
#include "duckdb/common/string.hpp"

#include "duckdb/common/identifier.hpp"
namespace duckdb_fork {
using namespace duckdb;
struct TriggerTableReferencingInfo {
	Identifier new_table;
	Identifier old_table;
};
} // namespace duckdb_fork
