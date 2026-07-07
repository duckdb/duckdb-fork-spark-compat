#include "duckdb/parser/tableref/showref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/peg/ast/partition_spec_entry.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"

namespace duckdb_fork {
using namespace duckdb;

// Build `SELECT * FROM <function_name>(<argument>)` — the shape every DESCRIBE form is rerouted into.
static unique_ptr<QueryNode> MakeDescribeSelect(const string &function_name, Value argument) {
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(make_uniq<ConstantExpression>(std::move(argument)));
	auto table_function = make_uniq<TableFunctionRef>();
	table_function->function = make_uniq<FunctionExpression>(Identifier(function_name), std::move(children));

	auto select_node = make_uniq<SelectNode>();
	select_node->select_list.push_back(make_uniq<StarExpression>());
	select_node->from_table = std::move(table_function);
	return std::move(select_node);
}

// Spark's DESCRIBE [TABLE] [EXTENDED|FORMATTED] is rerouted into a SELECT over an extension-registered table
// function (spark_describe / spark_describe_extended) producing the (col_name, data_type, comment) output.
static unique_ptr<QueryNode> BuildDescribeSelect(const DescribeTarget &describe_target, const string &function_name) {
	string target_name;
	if (describe_target.is_table_name) {
		target_name = describe_target.table_name.GetIdentifierName();
	} else if (describe_target.table_ref) {
		auto &base_table = *describe_target.table_ref;
		auto &qname = base_table.GetQualifiedName();
		if (!IsInvalidCatalog(qname.Catalog())) {
			target_name += qname.Catalog().GetIdentifierName() + ".";
		}
		if (!IsInvalidSchema(qname.Schema())) {
			target_name += qname.Schema().GetIdentifierName() + ".";
		}
		target_name += base_table.Table().GetIdentifierName();
	} else {
		throw ParserException("DESCRIBE requires a table or view name");
	}
	return MakeDescribeSelect(function_name, Value(target_name));
}

// Spark's DESCRIBE [QUERY] <query> is rerouted into a SELECT over the extension-registered spark_describe_query
// table function, which binds the query and reports its output schema (col_name, data_type, comment).
static unique_ptr<QueryNode> BuildDescribeQuerySelect(unique_ptr<SelectStatement> select_statement) {
	// Serialize the parsed query node so spark_describe_query can rebind the exact AST. (QueryNode::ToString() is
	// lossy: e.g. a DOUBLE literal like 10.00D renders as "10.0", which re-parses as DECIMAL.)
	MemoryStream stream;
	BinarySerializer::Serialize(*select_statement->node, stream);
	return MakeDescribeSelect("spark_describe_query", Value::BLOB(stream.GetData(), stream.GetPosition()));
}

// Spark's DESCRIBE FUNCTION [EXTENDED] <name> is rerouted into a SELECT over the extension-registered
// spark_describe_function[_extended] table function, which prints the function's metadata.
static unique_ptr<QueryNode> BuildDescribeFunctionSelect(const QualifiedName &function_name, bool extended) {
	string function_id = extended ? "spark_describe_function_extended" : "spark_describe_function";
	return MakeDescribeSelect(function_id, Value(function_name.Name().GetIdentifierName()));
}

// DescribeStatement <- ShowTables / ShowAllTables / DescribeQuery / DescribeTable / ShowSelect / ShowQualifiedName
// Hand-written: the DescribeTable/DescribeQuery alternatives make the generator skip this rule.
unique_ptr<SelectStatement> PEGTransformerFactory::TransformDescribeStatement(PEGTransformer &transformer, unique_ptr<QueryNode> child) {
	auto select_statement = make_uniq<SelectStatement>();
	select_statement->node = std::move(child);
	return select_statement;
}

unique_ptr<QueryNode>
PEGTransformerFactory::TransformShowSelect(PEGTransformer &transformer, const ShowType &show_or_describe_or_summarize,
                                           unique_ptr<SelectStatement> select_statement_internal) {
	// Spark's DESC <query> (no QUERY keyword) describes the query's output schema, same as DESCRIBE QUERY.
	if (show_or_describe_or_summarize == ShowType::DESCRIBE) {
		return BuildDescribeQuerySelect(std::move(select_statement_internal));
	}
	auto result = make_uniq<ShowRef>();
	result->show_type = show_or_describe_or_summarize;
	result->query = std::move(select_statement_internal->node);
	auto select_node = make_uniq<SelectNode>();
	select_node->select_list.push_back(make_uniq<StarExpression>());
	select_node->from_table = std::move(result);
	return std::move(select_node);
}

unique_ptr<QueryNode> PEGTransformerFactory::TransformShowTables(PEGTransformer &transformer,
                                                                 const ShowType &show_or_describe,
                                                                 const QualifiedName &qualified_name) {
	auto showref = make_uniq<ShowRef>();
	showref->show_type = ShowType::SHOW_FROM;
	if (!IsInvalidCatalog(qualified_name.Catalog())) {
		throw ParserException("Expected \"SHOW TABLES FROM database\", \"SHOW TABLES FROM schema\", or "
		                      "\"SHOW TABLES FROM database.schema\"");
	}
	if (IsInvalidSchema(qualified_name.Schema())) {
		showref->SetSchemaName(qualified_name.Name());
	} else {
		showref->SetCatalogName(qualified_name.Schema());
		showref->SetSchemaName(qualified_name.Name());
	}
	auto select_node = make_uniq<SelectNode>();
	select_node->select_list.push_back(make_uniq<StarExpression>());
	select_node->from_table = std::move(showref);
	return std::move(select_node);
}

unique_ptr<QueryNode> PEGTransformerFactory::TransformShowAllTables(PEGTransformer &transformer,
                                                                    const ShowType &show_or_describe) {
	auto result = make_uniq<ShowRef>();
	// Legacy reasons, see bind_showref.cpp
	result->SetTableName("__show_tables_expanded");
	result->show_type = ShowType::SHOW_UNQUALIFIED;
	auto select_node = make_uniq<SelectNode>();
	select_node->select_list.push_back(make_uniq<StarExpression>());
	select_node->from_table = std::move(result);
	return std::move(select_node);
}

unique_ptr<QueryNode> PEGTransformerFactory::TransformShowQualifiedName(PEGTransformer &transformer,
                                                                        const ShowType &show_or_describe_or_summarize,
                                                                        optional<DescribeTarget> describe_target) {
	auto showref = make_uniq<ShowRef>();
	showref->show_type = show_or_describe_or_summarize;
	DescribeTarget target;
	if (describe_target) {
		target = std::move(*describe_target);
	}

	if (target.is_table_name || target.table_ref) {
		if (target.is_table_name) {
			// Case: SHOW 'something' or DESCRIBE 'something'
			showref->SetTableName(target.table_name);
		} else {
			// Case: A relation/table reference
			auto &base_table = *target.table_ref;

			if (showref->show_type == ShowType::SHOW_FROM) {
				// Logic for SHOW TABLES FROM [database].[schema]
				if (IsInvalidSchema(base_table.GetQualifiedName().Schema())) {
					showref->SetSchemaName(base_table.Table());
				} else {
					showref->SetCatalogName(base_table.GetQualifiedName().Schema());
					showref->SetSchemaName(base_table.Table());
				}
			} else if (IsInvalidSchema(base_table.GetQualifiedName().Schema())) {
				// Logic for unqualified relations (databases, tables, variables)
				auto table_name = StringUtil::Lower(base_table.Table().GetIdentifierName());
				if (table_name == "databases" || table_name == "tables" || table_name == "schemas" ||
				    table_name == "variables") {
					showref->SetTableName(Identifier("\"" + table_name + "\""));
					showref->show_type = ShowType::SHOW_UNQUALIFIED;
				}
			}
		}
		if (showref->GetTableName().empty() && showref->show_type != ShowType::SHOW_FROM) {
			auto show_select_node = make_uniq<SelectNode>();
			show_select_node->select_list.push_back(make_uniq<StarExpression>());
			if (target.is_table_name) {
				// Case: SHOW 'something' or DESCRIBE 'something'
				auto table_ref = make_uniq<BaseTableRef>();
				table_ref->SetTable(target.table_name);
				show_select_node->from_table = std::move(table_ref);
			} else {
				// Case: A relation/table reference
				show_select_node->from_table = std::move(target.table_ref);
			}
			showref->query = std::move(show_select_node);
		}
	} else {
		// Case: No relation specified (e.g., just "SHOW TABLES")
		if (showref->show_type == ShowType::SUMMARY) {
			throw ParserException("Expected table name with SUMMARIZE");
		}
		showref->SetTableName("__show_tables_expanded");
		showref->show_type = ShowType::SHOW_UNQUALIFIED;
	}

	auto select_node = make_uniq<SelectNode>();
	select_node->select_list.push_back(make_uniq<StarExpression>());
	select_node->from_table = std::move(showref);

	return std::move(select_node);
}

DescribeTarget PEGTransformerFactory::TransformDescribeBaseTableName(PEGTransformer &transformer,
                                                                     unique_ptr<BaseTableRef> base_table_name) {
	DescribeTarget result;
	result.table_ref = std::move(base_table_name);
	return result;
}

DescribeTarget PEGTransformerFactory::TransformDescribeStringLiteral(PEGTransformer &transformer,
                                                                     const string &string_literal) {
	DescribeTarget result;
	result.is_table_name = true;
	result.table_name = Identifier(string_literal);
	return result;
}

ShowType PEGTransformerFactory::TransformSummarizeRule(PEGTransformer &transformer) {
	return ShowType::SUMMARY;
}

ShowType PEGTransformerFactory::TransformShowRule(PEGTransformer &transformer) {
	return ShowType::DESCRIBE;
}

ShowType PEGTransformerFactory::TransformDescribeLongRule(PEGTransformer &transformer) {
	return ShowType::DESCRIBE;
}

ShowType PEGTransformerFactory::TransformDescRule(PEGTransformer &transformer) {
	return ShowType::DESCRIBE;
}

// DescribeTable <- DescribeRule 'TABLE'? ('EXTENDED' / 'FORMATTED')? DescribeTarget PartitionSpec?
// Hand-written (the inlined keyword-choice modifier makes the generator skip this rule).
// partition_spec (DESC ... PARTITION (...)) is accepted and ignored — DuckDB has no per-partition describe, so we
// describe the whole table.
unique_ptr<QueryNode> PEGTransformerFactory::TransformDescribeTable(PEGTransformer &transformer, const ShowType &describe_rule, const bool &has_result, const bool &has_result_1, DescribeTarget describe_target, optional<vector<PartitionSpecEntry>> partition_spec) {
	// child 0: DescribeRule, child 1: optional 'TABLE', child 2: optional EXTENDED/FORMATTED, child 3: DescribeTarget
	bool extended = has_result_1;
	string function_name = extended ? "spark_describe_extended" : "spark_describe";
	return BuildDescribeSelect(describe_target, function_name);
}

// DescribeQuery <- DescribeRule 'QUERY' SelectStatementInternal
// Hand-written (referenced by DescribeStatement, which the generator skips).
unique_ptr<QueryNode> PEGTransformerFactory::TransformDescribeQuery(PEGTransformer &transformer, const ShowType &describe_rule, unique_ptr<SelectStatement> select_statement_internal) {
	return BuildDescribeQuerySelect(std::move(select_statement_internal));
}

// DescribeFunction <- DescribeRule 'FUNCTION' 'EXTENDED'? FunctionIdentifier
// Hand-written (referenced by DescribeStatement, which the generator skips).
unique_ptr<QueryNode> PEGTransformerFactory::TransformDescribeFunction(PEGTransformer &transformer, const ShowType &describe_rule, const bool &has_result, const QualifiedName &function_identifier) {
	bool extended = has_result;
	return BuildDescribeFunctionSelect(function_identifier, extended);
}

} // namespace duckdb_fork
