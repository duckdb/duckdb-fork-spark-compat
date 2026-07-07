#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/default_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/emptytableref.hpp"

namespace duckdb_fork {
using namespace duckdb;

static bool ExpressionReferencesColumn(const ParsedExpression &expr) {
	bool found = false;
	ParsedExpressionIterator::VisitExpressionClass(expr, ExpressionClass::COLUMN_REF,
	                                               [&](const ParsedExpression &) { found = true; });
	return found;
}

// ResetStatement <- 'RESET' SetVariableOrSetting
unique_ptr<SQLStatement> PEGTransformerFactory::TransformResetStatement(PEGTransformer &transformer,
                                                                        const SettingInfo &set_variable_or_setting) {
	if (set_variable_or_setting.scope == SetScope::LOCAL) {
		throw NotImplementedException("RESET LOCAL is not implemented.");
	}
	return make_uniq<ResetVariableStatement>(set_variable_or_setting.name, set_variable_or_setting.scope);
}

// ReadSettingStatement <- 'SET' SetVariableOrSetting
// Spark's value-less SET reads the config value; reroute it into SELECT current_setting('<key>').
unique_ptr<SQLStatement>
PEGTransformerFactory::TransformReadSettingStatement(PEGTransformer &transformer,
                                                     const SettingInfo &set_variable_or_setting) {
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(make_uniq<ConstantExpression>(Value(set_variable_or_setting.name.GetIdentifierName())));
	auto select_node = make_uniq<SelectNode>();
	select_node->select_list.push_back(make_uniq<FunctionExpression>(Identifier("current_setting"), std::move(children)));
	select_node->from_table = make_uniq<EmptyTableRef>();
	auto select_statement = make_uniq<SelectStatement>();
	select_statement->node = std::move(select_node);
	return std::move(select_statement);
}

// SetAssignment <- VariableAssign VariableList
vector<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformSetAssignment(PEGTransformer &transformer,
                                              vector<unique_ptr<ParsedExpression>> variable_list) {
	return variable_list;
}

// TODO: Mine was:
// // SetSetting <- DottedSettingIdentifier / (SettingScope? SettingName)
// // Complex rule (choice with a group) — skipped by the generator, so this is a hand-written entry point.
// SettingInfo PEGTransformerFactory::TransformSetSetting(PEGTransformer &transformer, ParseResult &parse_result) {
// 	auto &list_pr = parse_result.Cast<ListParseResult>();
// 	auto &result_pr = list_pr.Child<ChoiceParseResult>(0).GetResult();
//
// 	SettingInfo result;
// 	if (result_pr.name == "DottedSettingIdentifier") {
// 		// spark-style dotted setting, e.g. SET spark.sql.shuffle.partitions = 1
// 		result.name = Identifier(transformer.Transform<string>(result_pr));
// 		return result;
// 	}
// 	// (SettingScope? SettingName)
// 	auto &seq_pr = result_pr.Cast<ListParseResult>();
// 	auto &optional_scope_pr = seq_pr.Child<OptionalParseResult>(0);
// 	result.name = seq_pr.Child<IdentifierParseResult>(1).identifier;
// 	if (optional_scope_pr.HasResult()) {
// 		result.scope = transformer.Transform<SetScope>(optional_scope_pr.GetResult());
// 	}
// 	return result;
// }

// SetSetting <- DottedSettingIdentifier / (SettingScope? SettingName)
// Complex choice-with-group rule: skipped by the generator, so this is a hand-written entry point.
SettingInfo PEGTransformerFactory::TransformSetSetting(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	auto &result_pr = list_pr.Child<ChoiceParseResult>(0).GetResult();
	SettingInfo result;
	if (result_pr.name == "DottedSettingIdentifier") {
		// spark-style dotted setting, e.g. SET spark.sql.shuffle.partitions = 1
		result.name = Identifier(transformer.Transform<string>(result_pr));
		return result;
	}
	// (SettingScope? SettingName)
	auto &seq_pr = result_pr.Cast<ListParseResult>();
	auto &optional_scope_pr = seq_pr.Child<OptionalParseResult>(0);
	result.name = seq_pr.Child<IdentifierParseResult>(1).identifier;
	if (optional_scope_pr.HasResult()) {
		result.scope = transformer.Transform<SetScope>(optional_scope_pr.GetResult());
	}
	return result;
}

// DottedSettingIdentifier <- Identifier ('.' Identifier)+
string PEGTransformerFactory::TransformDottedSettingIdentifier(PEGTransformer &transformer,
                                                               const Identifier &identifier,
                                                               const vector<Identifier> &identifier_1) {
	string result = identifier.GetIdentifierName();
	for (auto &part : identifier_1) {
		result += ".";
		result += part;
	}
	return result;
}

// SetStatement <- 'SET' SetAssignmentOrTimeZone
unique_ptr<SQLStatement>
PEGTransformerFactory::TransformSetStatement(PEGTransformer &transformer,
                                             unique_ptr<SetStatement> set_assignment_or_time_zone) {
	return std::move(set_assignment_or_time_zone);
}

// ZoneLocal <- 'LOCAL'
unique_ptr<ParsedExpression> PEGTransformerFactory::TransformZoneLocal(PEGTransformer &transformer) {
	return make_uniq<DefaultExpression>();
}

// ZoneDefault <- 'DEFAULT'
unique_ptr<ParsedExpression> PEGTransformerFactory::TransformZoneDefault(PEGTransformer &transformer) {
	return make_uniq<DefaultExpression>();
}

// ZoneStringLiteral <- StringLiteral
unique_ptr<ParsedExpression> PEGTransformerFactory::TransformZoneStringLiteral(PEGTransformer &transformer,
                                                                               const string &string_literal) {
	return make_uniq<ConstantExpression>(Value(string_literal));
}

// ZoneIdentifier <- Identifier
unique_ptr<ParsedExpression> PEGTransformerFactory::TransformZoneIdentifier(PEGTransformer &transformer,
                                                                            const Identifier &identifier) {
	return make_uniq<ConstantExpression>(Value(identifier));
}

// SetTimeZone <- 'TIME' 'ZONE' ZoneValue
unique_ptr<SetStatement> PEGTransformerFactory::TransformSetTimeZone(PEGTransformer &transformer,
                                                                     unique_ptr<ParsedExpression> zone_value) {
	if (zone_value->GetExpressionClass() == ExpressionClass::DEFAULT) {
		return make_uniq<ResetVariableStatement>("timezone", SetScope::AUTOMATIC);
	}
	return make_uniq<SetVariableStatement>("timezone", std::move(zone_value), SetScope::AUTOMATIC);
}

// SetVariable <- VariableScope Identifier
SettingInfo PEGTransformerFactory::TransformSetVariable(PEGTransformer &transformer, const SetScope &variable_scope,
                                                        const Identifier &identifier) {
	SettingInfo result;
	result.name = identifier;
	result.scope = variable_scope;
	return result;
}

// StandardAssignment <- SetVariableOrSetting SetAssignment
unique_ptr<SetStatement>
PEGTransformerFactory::TransformStandardAssignment(PEGTransformer &transformer,
                                                   const SettingInfo &set_variable_or_setting,
                                                   vector<unique_ptr<ParsedExpression>> set_assignment) {
	if (set_variable_or_setting.scope == SetScope::LOCAL) {
		throw NotImplementedException("SET LOCAL is not implemented.");
	}
	if (set_assignment.size() > 1) {
		throw ParserException("SET can only contain a single value");
	}
	auto value = std::move(set_assignment[0]);
	if (value->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		// SET value cannot be a column reference
		auto &col_ref = value->Cast<ColumnRefExpression>();
		value = make_uniq<ConstantExpression>(col_ref.GetColumnName());
	} else if (value->GetExpressionClass() == ExpressionClass::DEFAULT) {
		return make_uniq<ResetVariableStatement>(set_variable_or_setting.name, set_variable_or_setting.scope);
	} else if (ExpressionReferencesColumn(*value)) {
		// Spark accepts arbitrary raw config values (e.g. SET k=org.apache.x.Y or SET k=UTF-8). These
		// tokenize into operator/struct expressions over column references, which the SET binder rejects.
		// Such pass-through settings are not interpreted, so store the value's textual form as a string.
		value = make_uniq<ConstantExpression>(Value(value->ToString()));
	}
	return make_uniq<SetVariableStatement>(set_variable_or_setting.name, std::move(value),
	                                       set_variable_or_setting.scope);
}

// VariableList <- List(Expression)
vector<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformVariableList(PEGTransformer &transformer,
                                             vector<unique_ptr<ParsedExpression>> expression) {
	return expression;
}

// VariableScope <- 'VARIABLE'
SetScope PEGTransformerFactory::TransformVariableScope(PEGTransformer &transformer) {
	return SetScope::VARIABLE;
}

// LocalScope <- 'LOCAL'
SetScope PEGTransformerFactory::TransformLocalScope(PEGTransformer &transformer) {
	return SetScope::LOCAL;
}

// SessionScope <- 'SESSION'
SetScope PEGTransformerFactory::TransformSessionScope(PEGTransformer &transformer) {
	return SetScope::SESSION;
}

// GlobalScope <- 'GLOBAL'
SetScope PEGTransformerFactory::TransformGlobalScope(PEGTransformer &transformer) {
	return SetScope::GLOBAL;
}

// ZoneIntervalWithInterval <- 'INTERVAL' StringLiteral Interval?
unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformZoneIntervalWithInterval(PEGTransformer &transformer, const string &string_literal,
                                                         const optional<DatePartSpecifier> &interval) {
	auto expr = make_uniq<ConstantExpression>(Value(string_literal));
	return make_uniq<CastExpression>(LogicalType::INTERVAL, std::move(expr));
}

// ZoneIntervalWithPrecision <- 'INTERVAL' Parens(NumberLiteral) StringLiteral
unique_ptr<ParsedExpression> PEGTransformerFactory::TransformZoneIntervalWithPrecision(
    PEGTransformer &transformer, unique_ptr<ParsedExpression> number_literal, const string &string_literal) {
	auto expr = make_uniq<ConstantExpression>(Value(string_literal));
	return make_uniq<CastExpression>(LogicalType::INTERVAL, std::move(expr));
}

} // namespace duckdb_fork
