#include "duckdb/parser/tableref/showref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"

namespace duckdb_fork {
using namespace duckdb;

// Spark's DESCRIBE [TABLE] [EXTENDED|FORMATTED] is rerouted into a SELECT over an extension-registered table
// function (spark_describe / spark_describe_extended) producing the (col_name, data_type, comment) output.
static unique_ptr<QueryNode> BuildDescribeSelect(const DescribeTarget &describe_target, const string &function_name) {
	string target_name;
	if (describe_target.is_table_name) {
		target_name = describe_target.table_name.GetIdentifierName();
	} else if (describe_target.table_ref) {
		auto &base_table = *describe_target.table_ref;
		if (!IsInvalidCatalog(base_table.catalog_name)) {
			target_name += base_table.catalog_name.GetIdentifierName() + ".";
		}
		if (!IsInvalidSchema(base_table.schema_name)) {
			target_name += base_table.schema_name.GetIdentifierName() + ".";
		}
		target_name += base_table.table_name.GetIdentifierName();
	} else {
		throw ParserException("DESCRIBE requires a table or view name");
	}

	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(make_uniq<ConstantExpression>(Value(target_name)));
	auto table_function = make_uniq<TableFunctionRef>();
	table_function->function = make_uniq<FunctionExpression>(Identifier(function_name), std::move(children));

	auto select_node = make_uniq<SelectNode>();
	select_node->select_list.push_back(make_uniq<StarExpression>());
	select_node->from_table = std::move(table_function);
	return std::move(select_node);
}

// Spark's DESCRIBE [QUERY] <query> is rerouted into a SELECT over the extension-registered spark_describe_query
// table function, which binds the query and reports its output schema (col_name, data_type, comment).
static unique_ptr<QueryNode> BuildDescribeQuerySelect(unique_ptr<SelectStatement> select_statement) {
	// Serialize the parsed query node so spark_describe_query can rebind the exact AST. (QueryNode::ToString() is
	// lossy: e.g. a DOUBLE literal like 10.00D renders as "10.0", which re-parses as DECIMAL.)
	MemoryStream stream;
	BinarySerializer::Serialize(*select_statement->node, stream);
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(make_uniq<ConstantExpression>(Value::BLOB(stream.GetData(), stream.GetPosition())));
	auto table_function = make_uniq<TableFunctionRef>();
	table_function->function = make_uniq<FunctionExpression>(Identifier("spark_describe_query"), std::move(children));

	auto select_node = make_uniq<SelectNode>();
	select_node->select_list.push_back(make_uniq<StarExpression>());
	select_node->from_table = std::move(table_function);
	return std::move(select_node);
}

// DescribeStatement <- ShowTables / ShowAllTables / DescribeQuery / DescribeTable / ShowSelect / ShowQualifiedName
// Hand-written: the DescribeTable/DescribeQuery alternatives make the generator skip this rule.
unique_ptr<SelectStatement> PEGTransformerFactory::TransformDescribeStatement(PEGTransformer &transformer,
                                                                              ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	auto &choice_pr = list_pr.Child<ChoiceParseResult>(0);
	auto child = transformer.Transform<unique_ptr<QueryNode>>(choice_pr.GetResult());
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
	if (!IsInvalidCatalog(qualified_name.catalog)) {
		throw ParserException("Expected \"SHOW TABLES FROM database\", \"SHOW TABLES FROM schema\", or "
		                      "\"SHOW TABLES FROM database.schema\"");
	}
	if (IsInvalidSchema(qualified_name.schema)) {
		showref->schema_name = qualified_name.name;
	} else {
		showref->catalog_name = qualified_name.schema;
		showref->schema_name = qualified_name.name;
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
	result->table_name = "__show_tables_expanded";
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
			showref->table_name = target.table_name;
		} else {
			// Case: A relation/table reference
			auto &base_table = *target.table_ref;

			if (showref->show_type == ShowType::SHOW_FROM) {
				// Logic for SHOW TABLES FROM [database].[schema]
				if (IsInvalidSchema(base_table.schema_name)) {
					showref->schema_name = base_table.table_name;
				} else {
					showref->catalog_name = base_table.schema_name;
					showref->schema_name = base_table.table_name;
				}
			} else if (IsInvalidSchema(base_table.schema_name)) {
				// Logic for unqualified relations (databases, tables, variables)
				auto table_name = StringUtil::Lower(base_table.table_name.GetIdentifierName());
				if (table_name == "databases" || table_name == "tables" || table_name == "schemas" ||
				    table_name == "variables") {
					showref->table_name = Identifier("\"" + table_name + "\"");
					showref->show_type = ShowType::SHOW_UNQUALIFIED;
				}
			}
		}
		if (showref->table_name.empty() && showref->show_type != ShowType::SHOW_FROM) {
			auto show_select_node = make_uniq<SelectNode>();
			show_select_node->select_list.push_back(make_uniq<StarExpression>());
			if (target.is_table_name) {
				// Case: SHOW 'something' or DESCRIBE 'something'
				auto table_ref = make_uniq<BaseTableRef>();
				table_ref->table_name = target.table_name;
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
		showref->table_name = "__show_tables_expanded";
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

// DescribeTable <- DescribeRule 'TABLE'? ('EXTENDED' / 'FORMATTED')? DescribeTarget
// Hand-written (the inlined keyword-choice modifier makes the generator skip this rule).
unique_ptr<QueryNode> PEGTransformerFactory::TransformDescribeTable(PEGTransformer &transformer,
                                                                    ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	// child 0: DescribeRule, child 1: optional 'TABLE', child 2: optional EXTENDED/FORMATTED, child 3: DescribeTarget
	bool extended = list_pr.Child<OptionalParseResult>(2).HasResult();
	auto describe_target = transformer.Transform<DescribeTarget>(list_pr.GetChild(3));
	string function_name = extended ? "spark_describe_extended" : "spark_describe";
	return BuildDescribeSelect(describe_target, function_name);
}

// DescribeQuery <- DescribeRule 'QUERY' SelectStatementInternal
// Hand-written (referenced by DescribeStatement, which the generator skips).
unique_ptr<QueryNode> PEGTransformerFactory::TransformDescribeQuery(PEGTransformer &transformer,
                                                                    ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	// child 0: DescribeRule, child 1: 'QUERY', child 2: SelectStatementInternal
	auto select_statement = transformer.Transform<unique_ptr<SelectStatement>>(list_pr.GetChild(2));
	return BuildDescribeQuerySelect(std::move(select_statement));
}

} // namespace duckdb_fork
