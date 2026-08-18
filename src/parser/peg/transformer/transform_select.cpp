#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression_map.hpp"
#include "duckdb/parser/peg/ast/distinct_clause.hpp"
#include "duckdb/parser/peg/ast/join_prefix.hpp"
#include "duckdb/parser/peg/ast/join_qualifier.hpp"
#include "duckdb/parser/peg/ast/limit_percent_result.hpp"
#include "duckdb/parser/peg/ast/table_alias.hpp"
#include "duckdb/parser/tableref/showref.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/tableref/emptytableref.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/expressionlistref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/parser/tableref/at_clause.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/tableref/pivotref.hpp"
#include "duckdb/parser/statement/insert_statement.hpp"
#include "duckdb/parser/statement/update_statement.hpp"
#include "duckdb/parser/statement/delete_statement.hpp"
#include "duckdb/parser/query_node/insert_query_node.hpp"
#include "duckdb/parser/query_node/update_query_node.hpp"
#include "duckdb/parser/query_node/delete_query_node.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/lambda_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/main/query_result.hpp"

namespace duckdb {

namespace {

//! Render an expression's Spark auto-generated output column name (covers column refs, constants and
//! function calls). Returns nullopt for kinds we don't render, so such expressions never match.
optional<string> SparkColumnName(const ParsedExpression &expr) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::COLUMN_REF:
		return expr.Cast<ColumnRefExpression>().GetColumnName().GetIdentifierName();
	case ExpressionClass::CONSTANT:
		return expr.Cast<ConstantExpression>().GetValue().ToString();
	case ExpressionClass::FUNCTION: {
		auto &func = expr.Cast<FunctionExpression>();
		vector<string> child_names;
		for (auto &arg : func.GetArguments()) {
			if (arg.HasName()) {
				return optional<string>();
			}
			auto child_name = SparkColumnName(arg.GetExpression());
			if (!child_name.has_value()) {
				return optional<string>();
			}
			child_names.push_back(std::move(*child_name));
		}
		auto function_name = StringUtil::Lower(func.FunctionName().GetIdentifierName());
		if (func.IsOperator()) {
			// operators render infix and parenthesized: `d + 1` is named "(d + 1)"
			if (child_names.size() == 1) {
				return "(" + function_name + " " + child_names[0] + ")";
			}
			if (child_names.size() == 2) {
				return "(" + child_names[0] + " " + function_name + " " + child_names[1] + ")";
			}
			return optional<string>();
		}
		string result = function_name + "(";
		for (idx_t i = 0; i < child_names.size(); i++) {
			result += i == 0 ? child_names[i] : ", " + child_names[i];
		}
		return result + ")";
	}
	default:
		return optional<string>();
	}
}

//! Strip all whitespace and lowercase, so a clause reference matches a Spark auto-name regardless of
//! spacing/case (back-tick identifiers arrive whitespace-collapsed).
string NormalizeAutoName(const string &name) {
	string result;
	for (auto c : name) {
		if (!StringUtil::CharacterIsSpace(c)) {
			result += c;
		}
	}
	return StringUtil::Lower(result);
}

//! The auto-name a bare (single-part) column reference names, if any.
optional_idx MatchAutoName(const ParsedExpression &expr, const unordered_map<string, idx_t> &auto_names) {
	if (expr.GetExpressionClass() != ExpressionClass::COLUMN_REF) {
		return optional_idx();
	}
	auto &col = expr.Cast<ColumnRefExpression>();
	if (col.IsQualified()) {
		return optional_idx();
	}
	auto entry = auto_names.find(NormalizeAutoName(col.GetColumnName().GetIdentifierName()));
	if (entry == auto_names.end()) {
		return optional_idx();
	}
	return entry->second;
}

//! If `expr` names the Spark auto-name of an unaliased select item, resolve it to that item. A
//! constant match becomes a 1-based positional reference: a copy of the constant would be read as a
//! positional ordinal by value (e.g. GROUP BY 7 -> 7th column). A function match is replaced with a
//! copy of the select expression.
void RewriteAutoNameRef(unique_ptr<ParsedExpression> &expr, const vector<unique_ptr<ParsedExpression>> &select_list,
                        const unordered_map<string, idx_t> &auto_names) {
	if (!expr) {
		return;
	}
	auto match = MatchAutoName(*expr, auto_names);
	if (!match.IsValid()) {
		return;
	}
	auto &matched = *select_list[match.GetIndex()];
	if (matched.GetExpressionClass() == ExpressionClass::CONSTANT) {
		expr = make_uniq<ConstantExpression>(Value::INTEGER(UnsafeNumericCast<int32_t>(match.GetIndex() + 1)));
	} else {
		expr = matched.Copy();
	}
}

//! Spark lets GROUP BY / ORDER BY reference an unaliased select item by its auto-generated output
//! name. DuckDB only matches explicit aliases, so resolve such references here by rewriting them to
//! the matching select item. Function calls are always eligible: their auto-names contain characters
//! that cannot appear in an unquoted identifier, so they never collide with a real column. Constants
//! are eligible only when there is no FROM clause: a constant auto-name such as "1" can collide with a
//! real column and would shadow it, but with no FROM there are no real columns. Plain column
//! references are left to DuckDB's normal resolution.
void RewriteSparkAutoNameReferences(SelectNode &node) {
	bool no_from = node.from_table && node.from_table->type == TableReferenceType::EMPTY_FROM;
	unordered_map<string, idx_t> auto_names;
	for (idx_t i = 0; i < node.select_list.size(); i++) {
		auto &select_expr = node.select_list[i];
		if (select_expr->HasAlias()) {
			continue;
		}
		auto expr_class = select_expr->GetExpressionClass();
		if (expr_class != ExpressionClass::FUNCTION && !(no_from && expr_class == ExpressionClass::CONSTANT)) {
			continue;
		}
		auto name = SparkColumnName(*select_expr);
		if (name.has_value()) {
			auto_names.emplace(NormalizeAutoName(*name), i);
		}
	}
	if (auto_names.empty()) {
		return;
	}
	for (auto &group_expr : node.groups.group_expressions) {
		RewriteAutoNameRef(group_expr, node.select_list, auto_names);
	}
	for (auto &modifier : node.modifiers) {
		if (modifier->type != ResultModifierType::ORDER_MODIFIER) {
			continue;
		}
		for (auto &order : modifier->Cast<OrderModifier>().orders) {
			RewriteAutoNameRef(order.expression, node.select_list, auto_names);
		}
	}
}

//! Spark's GROUPING SETS / CUBE / ROLLUP rewrite aliases every unaliased group expression to its
//! auto-name, and that alias - not the underlying columns - is what an ORDER BY above the aggregate
//! sees, so GROUP BY GROUPING SETS (a, d + 1) ORDER BY `(d + 1)` resolves. DuckDB has no such name, so
//! rewrite the reference to the group expression itself. Only function calls (operators included) are
//! registered: a column reference keeps its own name and already resolves, and a constant group
//! expression is a positional ordinal rather than a value.
void RewriteSparkGroupByAliasReferences(SelectNode &node) {
	auto &group_expressions = node.groups.group_expressions;
	vector<reference<OrderModifier>> order_modifiers;
	for (auto &modifier : node.modifiers) {
		if (modifier->type == ResultModifierType::ORDER_MODIFIER) {
			order_modifiers.push_back(modifier->Cast<OrderModifier>());
		}
	}
	if (group_expressions.empty() || order_modifiers.empty()) {
		return;
	}
	unordered_map<string, idx_t> alias_names;
	for (idx_t i = 0; i < group_expressions.size(); i++) {
		if (group_expressions[i]->GetExpressionClass() != ExpressionClass::FUNCTION) {
			continue;
		}
		auto name = SparkColumnName(*group_expressions[i]);
		if (name.has_value()) {
			alias_names.emplace(NormalizeAutoName(*name), i);
		}
	}
	if (alias_names.empty()) {
		return;
	}
	for (auto &order_modifier : order_modifiers) {
		for (auto &order : order_modifier.get().orders) {
			if (!order.expression) {
				continue;
			}
			auto match = MatchAutoName(*order.expression, alias_names);
			if (match.IsValid()) {
				order.expression = group_expressions[match.GetIndex()]->Copy();
			}
		}
	}
}

//! A spark generator call is only rewritten in its bare form: no alias, no schema qualification, no
//! aggregate decorations, and only positional arguments.
bool IsUndecoratedGeneratorCall(const FunctionExpression &func) {
	if (!func.GetAlias().empty() || !func.GetQualifiedName().Schema().empty() || func.Distinct() || func.Filter() ||
	    !func.OrderBy()->orders.empty()) {
		return false;
	}
	for (auto &arg : func.GetArguments()) {
		if (arg.HasName()) {
			return false;
		}
	}
	return true;
}

//! The collection generators additionally take exactly one argument.
bool IsBareGeneratorCall(const FunctionExpression &func) {
	return IsUndecoratedGeneratorCall(func) && func.GetArguments().size() == 1;
}

//! The __spark_*_entries helper that yields a generator's rows as a LIST of STRUCTs, one struct per output
//! row and one struct field per output column, or nullptr if the name is not a rewritable spark generator.
const char *GeneratorEntriesFunction(const string &lower_name) {
	if (lower_name == "explode") {
		return "__spark_explode_entries";
	}
	if (lower_name == "explode_outer") {
		return "__spark_explode_outer_entries";
	}
	if (lower_name == "posexplode") {
		return "__spark_posexplode_entries";
	}
	if (lower_name == "posexplode_outer") {
		return "__spark_posexplode_outer_entries";
	}
	if (lower_name == "inline") {
		return "__spark_inline_entries";
	}
	if (lower_name == "inline_outer") {
		return "__spark_inline_outer_entries";
	}
	return nullptr;
}

//! `unnest(<rows>, max_depth := 2)`: unnest the generator's row list and expand each row struct into the
//! output columns, without descending into a nested collection inside a column.
unique_ptr<ParsedExpression> ExpandGeneratorRows(unique_ptr<ParsedExpression> rows) {
	auto max_depth = make_uniq<ConstantExpression>(Value::INTEGER(2));
	max_depth->SetAlias(Identifier("max_depth"));
	vector<unique_ptr<ParsedExpression>> unnest_args;
	unnest_args.push_back(std::move(rows));
	unnest_args.push_back(std::move(max_depth));
	return make_uniq<FunctionExpression>(Identifier("unnest"), std::move(unnest_args));
}

//! Spark's json_tuple(json, k1, ..., kn) is a generator that extracts n keys into n output columns named
//! c0..c{n-1}, one output row per input row. Build that row as a STRUCT of one __spark_json_tuple_value call
//! per key; the root unnest then expands it into the output columns. A plain unnest suffices because every
//! field is a string, so there is no nested collection for a deeper unnest to descend into.
unique_ptr<ParsedExpression> RewriteJsonTupleGenerator(FunctionExpression &func) {
	auto &args = func.GetArgumentsMutable();
	vector<FunctionArgument> row_fields;
	for (idx_t i = 1; i < args.size(); i++) {
		vector<unique_ptr<ParsedExpression>> value_args;
		value_args.push_back(args[0].GetExpression().Copy());
		value_args.push_back(std::move(args[i].GetExpressionMutable()));
		row_fields.emplace_back(
		    Identifier("c" + to_string(i - 1)),
		    make_uniq<FunctionExpression>(Identifier("__spark_json_tuple_value"), std::move(value_args)));
	}
	vector<unique_ptr<ParsedExpression>> unnest_args;
	unnest_args.push_back(make_uniq<FunctionExpression>(Identifier("struct_pack"), std::move(row_fields)));
	return make_uniq<FunctionExpression>(Identifier("unnest"), std::move(unnest_args));
}

//! Spark's stack(n, e1, ..., ek) is a generator that lays the k values out row-major into n rows of
//! ceil(k/n) columns named col0..col{numFields-1}, filling any slot past the last value with NULL. n is a
//! foldable integer, so the row shape is known here: build the rows as a list of structs whose fields the
//! root unnest expands into the output columns, with list_value unifying each column's type across rows.
//! Rows holding no value at all come from list_resize, whose NULL list elements the struct-expanding unnest
//! turns into all-NULL rows; that keeps the rewrite bounded by the arguments written even for a large n.
unique_ptr<ParsedExpression> RewriteStackGenerator(FunctionExpression &func) {
	auto &args = func.GetArgumentsMutable();
	auto &row_count_expr = args[0].GetExpression();
	if (row_count_expr.GetExpressionClass() != ExpressionClass::CONSTANT) {
		return nullptr;
	}
	auto &row_count_value = row_count_expr.Cast<ConstantExpression>().GetValue();
	Value row_count_bigint;
	if (row_count_value.IsNull() || !row_count_value.type().IsIntegral() ||
	    !row_count_value.DefaultTryCastAs(LogicalType::BIGINT, row_count_bigint, nullptr)) {
		return nullptr;
	}
	auto row_count = row_count_bigint.GetValue<int64_t>();
	if (row_count < 1) {
		return nullptr;
	}
	auto num_values = args.size() - 1;
	auto num_rows = NumericCast<idx_t>(row_count);
	auto num_fields = num_rows >= num_values ? 1 : (num_values + num_rows - 1) / num_rows;
	auto num_value_rows = (num_values + num_fields - 1) / num_fields;
	vector<unique_ptr<ParsedExpression>> rows;
	for (idx_t row = 0; row < num_value_rows; row++) {
		vector<FunctionArgument> row_fields;
		for (idx_t col = 0; col < num_fields; col++) {
			auto index = row * num_fields + col;
			unique_ptr<ParsedExpression> field;
			if (index < num_values) {
				field = std::move(args[index + 1].GetExpressionMutable());
			} else {
				field = make_uniq<ConstantExpression>(Value());
			}
			row_fields.emplace_back(Identifier("col" + to_string(col)), std::move(field));
		}
		rows.push_back(make_uniq<FunctionExpression>(Identifier("struct_pack"), std::move(row_fields)));
	}
	unique_ptr<ParsedExpression> list = make_uniq<FunctionExpression>(Identifier("list_value"), std::move(rows));
	if (num_value_rows < num_rows) {
		vector<unique_ptr<ParsedExpression>> resize_args;
		resize_args.push_back(std::move(list));
		resize_args.push_back(make_uniq<ConstantExpression>(Value::BIGINT(row_count)));
		list = make_uniq<FunctionExpression>(Identifier("list_resize"), std::move(resize_args));
	}
	return ExpandGeneratorRows(std::move(list));
}

//! Spark's stack doubles as a table-valued function, where the TVF form is the same generator over a single
//! row (FunctionRegistry.scala builds Generate(<generator>, child = OneRowRelation()) for it). Reuse the
//! select-item rewrite by wrapping its expression in a subquery, so one implementation serves both call
//! positions; the table alias and its column alias list carry over. Correlated arguments keep working
//! because a subquery in FROM resolves outer columns the same way the table function form does.
unique_ptr<TableRef> RewriteStackTableFunction(TableFunctionRef &ref) {
	if (ref.with_ordinality == OrdinalityType::WITH_ORDINALITY ||
	    ref.function->GetExpressionClass() != ExpressionClass::FUNCTION) {
		return nullptr;
	}
	auto &func = ref.function->Cast<FunctionExpression>();
	if (StringUtil::Lower(func.FunctionName().GetIdentifierName()) != "stack") {
		return nullptr;
	}
	if (func.GetArguments().size() < 2 || !IsUndecoratedGeneratorCall(func)) {
		return nullptr;
	}
	auto rows = RewriteStackGenerator(func);
	if (!rows) {
		return nullptr;
	}
	auto select_node = make_uniq<SelectNode>();
	select_node->select_list.push_back(std::move(rows));
	select_node->from_table = make_uniq<EmptyTableRef>();
	auto select_statement = make_uniq<SelectStatement>();
	select_statement->node = std::move(select_node);
	auto subquery = make_uniq<SubqueryRef>(std::move(select_statement));
	subquery->alias = ref.alias;
	subquery->column_name_alias = ref.column_name_alias;
	return std::move(subquery);
}

//! Spark's explode()/posexplode()/inline() and their _outer variants are generators in SELECT position: one
//! output row per element, arrays yielding a single column, maps key/value columns and arrays of structs one
//! column per struct field, with the posexplode variants adding a leading element position column. Rewrite a
//! top-level, unaliased generator select item to `unnest(<entries>(x), max_depth := 2)` so the struct-expanding
//! unnest sits at the select-item root - a scalar macro body binds as a non-root expression, which rejects the
//! struct expansion - while the entries function dispatches the output column shape (the _outer helpers
//! additionally emit one all-NULL row for a NULL/empty collection).
//! Aliased calls are left to the scalar explode/explode_outer macros, which unnest a list under the alias.
void RewriteSparkSelectGenerators(SelectNode &node) {
	for (auto &select_expr : node.select_list) {
		if (select_expr->GetExpressionClass() != ExpressionClass::FUNCTION) {
			continue;
		}
		auto &func = select_expr->Cast<FunctionExpression>();
		auto lower_name = StringUtil::Lower(func.FunctionName().GetIdentifierName());
		if (lower_name == "json_tuple") {
			if (func.GetArguments().size() >= 2 && IsUndecoratedGeneratorCall(func)) {
				select_expr = RewriteJsonTupleGenerator(func);
			}
			continue;
		}
		if (lower_name == "stack") {
			// the row count plus at least one value; a non-constant row count is left unrewritten
			if (func.GetArguments().size() >= 2 && IsUndecoratedGeneratorCall(func)) {
				auto stack_rows = RewriteStackGenerator(func);
				if (stack_rows) {
					select_expr = std::move(stack_rows);
				}
			}
			continue;
		}
		auto entries_fn = GeneratorEntriesFunction(lower_name);
		if (!entries_fn || !IsBareGeneratorCall(func)) {
			continue;
		}
		vector<unique_ptr<ParsedExpression>> entries_args;
		entries_args.push_back(std::move(func.GetArgumentsMutable()[0].GetExpressionMutable()));
		select_expr =
		    ExpandGeneratorRows(make_uniq<FunctionExpression>(Identifier(entries_fn), std::move(entries_args)));
	}
}

//! The select node of a table reference of the form `(SELECT ...)`.
optional_ptr<SelectNode> SubquerySelect(TableRef &ref) {
	if (ref.type != TableReferenceType::SUBQUERY) {
		return nullptr;
	}
	auto &subquery = ref.Cast<SubqueryRef>();
	if (!subquery.subquery || subquery.subquery->node->type != QueryNodeType::SELECT_NODE) {
		return nullptr;
	}
	return subquery.subquery->node->Cast<SelectNode>();
}

//! The select node of a `(SELECT ...)` table reference without a FROM clause, which is where a star has no
//! input columns of its own to expand over.
optional_ptr<SelectNode> FromlessSubquerySelect(TableRef &ref) {
	auto select_node = SubquerySelect(ref);
	if (!select_node || !select_node->from_table || select_node->from_table->type != TableReferenceType::EMPTY_FROM) {
		return nullptr;
	}
	return select_node;
}

//! The relation names a star can name in a query's own FROM clause. Returns false when a reference is bound
//! under a name the query does not spell out, since the collected names then say nothing about what is in
//! scope and no conclusion can be drawn from a star's name being absent from them.
bool CollectFromRelationNames(const TableRef &ref, identifier_set_t &names) {
	if (!ref.alias.empty()) {
		names.insert(ref.alias);
	}
	switch (ref.type) {
	case TableReferenceType::EMPTY_FROM:
		return true;
	case TableReferenceType::BASE_TABLE:
		// an alias hides the table name, but collecting both only ever suppresses a rewrite
		names.insert(ref.Cast<BaseTableRef>().Table());
		return true;
	case TableReferenceType::JOIN: {
		auto &join = ref.Cast<JoinRef>();
		return join.left && join.right && CollectFromRelationNames(*join.left, names) &&
		       CollectFromRelationNames(*join.right, names);
	}
	case TableReferenceType::SUBQUERY:
	case TableReferenceType::EXPRESSION_LIST:
	case TableReferenceType::TABLE_FUNCTION:
	case TableReferenceType::PIVOT:
		// without an alias the binder names these itself (`unnamed_subquery`, `unnamed_query1`, ...)
		return !ref.alias.empty();
	default:
		return false;
	}
}

//! A star that stands for the input columns as they are: no COLUMNS, no EXCLUDE/REPLACE/RENAME.
bool IsPlainStar(const ParsedExpression &expr) {
	if (!StarExpression::IsStar(expr)) {
		return false;
	}
	auto &star = expr.Cast<StarExpression>();
	return !star.Expression() && star.ExcludeList().empty() && star.ReplaceList().empty() && star.RenameList().empty();
}

//! A star that names a relation and carries nothing unnest cannot express. An unqualified star has no
//! relation to unnest, and EXCLUDE/REPLACE/RENAME have no unnest equivalent.
bool IsPlainQualifiedStar(const ParsedExpression &expr) {
	return IsPlainStar(expr) && !expr.Cast<StarExpression>().RelationName().empty();
}

//! A bare `*`: every column the query's own FROM clause provides, unchanged.
bool IsPlainUnqualifiedStar(const ParsedExpression &expr) {
	return IsPlainStar(expr) && expr.Cast<StarExpression>().RelationName().empty();
}

//! A star expands over the input columns of its query, so a star qualified by a name the query's own FROM
//! clause does not provide can only name a relation of the enclosing query: in
//! `LATERAL (SELECT t1.*, t2.* FROM t2)`, `t1` is the outer relation and the star stands for a copy of the
//! current t1 row. duckdb expands a star from the bindings of its own query, which never hold an outer
//! relation, so such a star is lowered to unnest over that relation's row struct - a column reference, and
//! those do resolve against the enclosing query. TransformStarExpression uses the same lowering for a star
//! with more qualifiers than a relation name can hold (`db.tbl.*`).
void RewriteOuterQualifiedStars(TableRef &ref) {
	auto select_node = SubquerySelect(ref);
	if (!select_node || !select_node->from_table) {
		return;
	}
	bool has_qualified_star = false;
	for (auto &select_expr : select_node->select_list) {
		if (IsPlainQualifiedStar(*select_expr)) {
			has_qualified_star = true;
			break;
		}
	}
	if (!has_qualified_star) {
		return;
	}
	identifier_set_t local_relations;
	if (!CollectFromRelationNames(*select_node->from_table, local_relations)) {
		return;
	}
	for (auto &select_expr : select_node->select_list) {
		if (!IsPlainQualifiedStar(*select_expr)) {
			continue;
		}
		auto &relation_name = select_expr->Cast<StarExpression>().RelationName();
		if (local_relations.find(relation_name) != local_relations.end()) {
			continue;
		}
		vector<unique_ptr<ParsedExpression>> children;
		children.push_back(make_uniq<ColumnRefExpression>(relation_name));
		auto unnest = make_uniq<FunctionExpression>("unnest", std::move(children));
		unnest->SetQueryLocation(select_expr->GetQueryLocation());
		select_expr = std::move(unnest);
	}
}

//! The expression that decides a group key's identity: an ordinal stands for the select item it names.
//! An out-of-range ordinal stands for itself, so the binder still reports it.
const ParsedExpression &GroupByKeyExpression(const ParsedExpression &group_expr,
                                             const vector<unique_ptr<ParsedExpression>> &select_list) {
	if (group_expr.GetExpressionClass() != ExpressionClass::CONSTANT) {
		return group_expr;
	}
	auto &value = group_expr.Cast<ConstantExpression>().GetValue();
	if (value.IsNull() || !value.type().IsIntegral()) {
		return group_expr;
	}
	auto ordinal = value.GetValue<int64_t>();
	if (ordinal < 1 || static_cast<idx_t>(ordinal) > select_list.size()) {
		return group_expr;
	}
	return *select_list[static_cast<idx_t>(ordinal) - 1];
}

//! Spark resolves a GROUP BY ordinal against the select list before grouping sets are formed, so an
//! ordinal and the select item it names are one grouping key. Group keys are numbered here by structural
//! equality of the raw expressions while ordinals are only resolved later, in the binder, which leaves
//! GROUPING SETS ((1), (b), (a, 2)) with two keys for `a` and two for `b` - the select list can reference
//! only one of each, so the sets built on the other keys project NULL. Merge the keys that become equal
//! once ordinals are canonicalized to their select item, and remap the grouping sets onto them. Idempotent,
//! so it can run again after a rewrite that turns a group expression into one of the select items.
void MergeDuplicateGroupByKeys(SelectNode &node) {
	auto &group_expressions = node.groups.group_expressions;
	if (group_expressions.size() < 2 || node.groups.grouping_sets.empty()) {
		return;
	}
	for (auto &select_expr : node.select_list) {
		// an ordinal into an unexpanded star only resolves once the binder has expanded it
		if (select_expr->GetExpressionClass() == ExpressionClass::STAR) {
			return;
		}
	}
	parsed_expression_map_t<idx_t> key_indexes;
	vector<idx_t> remap;
	remap.reserve(group_expressions.size());
	for (auto &group_expr : group_expressions) {
		auto &key = GroupByKeyExpression(*group_expr, node.select_list);
		auto entry = key_indexes.find(key);
		if (entry != key_indexes.end()) {
			remap.push_back(entry->second);
			continue;
		}
		auto key_index = key_indexes.size();
		key_indexes[key] = key_index;
		remap.push_back(key_index);
	}
	if (key_indexes.size() == group_expressions.size()) {
		return;
	}
	vector<unique_ptr<ParsedExpression>> merged;
	merged.reserve(key_indexes.size());
	for (idx_t i = 0; i < group_expressions.size(); i++) {
		if (remap[i] == merged.size()) {
			merged.push_back(std::move(group_expressions[i]));
		}
	}
	group_expressions = std::move(merged);
	for (auto &grouping_set : node.groups.grouping_sets) {
		GroupingSet remapped_set;
		for (auto &index : grouping_set) {
			remapped_set.insert(ProjectionIndex(remap[index.GetIndex()]));
		}
		grouping_set = std::move(remapped_set);
	}
}

//! GROUP BY ALL arrives as a single star expression - drop it and let the binder infer the groups.
void AssignGroupByNode(SelectNode &node, GroupByNode groups) {
	if (groups.group_expressions.size() == 1 &&
	    PEGTransformerFactory::ExpressionIsEmptyStar(*groups.group_expressions[0])) {
		node.aggregate_handling = AggregateHandling::FORCE_AGGREGATES;
		groups.group_expressions.clear();
		groups.grouping_sets.clear();
	}
	node.groups = std::move(groups);
	MergeDuplicateGroupByKeys(node);
}

} // namespace

unique_ptr<SQLStatement>
PEGTransformerFactory::TransformSelectStatement(PEGTransformer &transformer,
                                                unique_ptr<SelectStatement> select_statement_internal) {
	return std::move(select_statement_internal);
}

unique_ptr<SelectStatement> PEGTransformerFactory::TransformSelectStatementInternalRule(PEGTransformer &transformer,
                                                                                        ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	CommonTableExpressionMap cte_map;
	transformer.TransformOptional<CommonTableExpressionMap>(list_pr, 0, cte_map);
	if (!cte_map.map.empty()) {
		transformer.stored_cte_map.push_back(cte_map);
	}
	auto select_statement = transformer.Transform<unique_ptr<SelectStatement>>(list_pr.Child<ListParseResult>(1));

	if (!cte_map.map.empty()) {
		select_statement->node->cte_map = std::move(cte_map);
	}
	vector<unique_ptr<ResultModifier>> result_modifiers;
	transformer.TransformOptional<vector<unique_ptr<ResultModifier>>>(list_pr, 2, result_modifiers);
	for (auto &result_modifier : result_modifiers) {
		select_statement->node->modifiers.push_back(std::move(result_modifier));
	}
	if (select_statement->node->type != QueryNodeType::SELECT_NODE) {
		return select_statement;
	}
	auto &select_node = select_statement->node->Cast<SelectNode>();
	RewriteSparkAutoNameReferences(select_node);
	RewriteSparkGroupByAliasReferences(select_node);
	RewriteSparkSelectGenerators(select_node);
	// the auto-name rewrite can turn a group expression into a copy of a select item it shares a key with
	MergeDuplicateGroupByKeys(select_node);
	if (select_node.from_table->type != TableReferenceType::SHOW_REF) {
		return select_statement;
	}
	auto &show_ref = select_node.from_table->Cast<ShowRef>();
	if (!select_statement->node->cte_map.map.empty()) {
		throw ParserException("%s with CTE not allowed - wrap the statement in a subquery instead",
		                      EnumUtil::ToString(show_ref.show_type));
	}
	if (!select_statement->node->modifiers.empty()) {
		throw ParserException("%s with ORDER BY not allowed - wrap the statement in a subquery instead",
		                      EnumUtil::ToString(show_ref.show_type));
	}
	return select_statement;
}

static void PushSelectStatementInternalRemainder(TransformStack &stack, TransformStackFrame &frame) {
	auto &list_pr = frame.parse_result.Cast<ListParseResult>();
	auto &result_modifiers_opt = list_pr.Child<OptionalParseResult>(2);
	if (result_modifiers_opt.HasResult()) {
		stack.PushFrame(result_modifiers_opt.GetResult(), PEGTransformerFactory::GetTrampolineOps("ResultModifiers"),
		                TransformFrameResultTarget(frame.frame_index, 2));
	}
	stack.PushFrame(list_pr.GetChild(1), PEGTransformerFactory::GetTrampolineOps("PipeOperatorChain"),
	                TransformFrameResultTarget(frame.frame_index, 1));
}

void PEGTransformerFactory::InitializeSelectStatementInternalTrampoline(PEGTransformer &transformer,
                                                                        TransformStack &stack,
                                                                        TransformStackFrame &frame) {
	auto &list_pr = frame.parse_result.Cast<ListParseResult>();
	frame.ReserveChildSlots(3);
	auto &with_clause_opt = list_pr.Child<OptionalParseResult>(0);
	if (with_clause_opt.HasResult()) {
		frame.manual_state = 0;
		stack.PushFrame(with_clause_opt.GetResult(), PEGTransformerFactory::GetTrampolineOps("WithClause"),
		                TransformFrameResultTarget(frame.frame_index, 0));
		return;
	}
	frame.manual_state = 1;
	PushSelectStatementInternalRemainder(stack, frame);
}

unique_ptr<TransformResultValue>
PEGTransformerFactory::FinalizeSelectStatementInternalTrampoline(PEGTransformer &transformer, TransformStack &stack,
                                                                 TransformStackFrame &frame) {
	if (frame.manual_state == 0) {
		auto &cte_map = frame.GetResult<CommonTableExpressionMap>(0);
		if (!cte_map.map.empty()) {
			transformer.stored_cte_map.push_back(cte_map);
		}
		frame.manual_state = 1;
		PushSelectStatementInternalRemainder(stack, frame);
		return nullptr;
	}

	CommonTableExpressionMap cte_map;
	if (frame.child_results[0]) {
		cte_map = frame.TakeResult<CommonTableExpressionMap>(0);
	}
	auto select_statement = frame.TakeResult<unique_ptr<SelectStatement>>(1);
	if (!cte_map.map.empty()) {
		select_statement->node->cte_map = std::move(cte_map);
	}
	if (frame.child_results[2]) {
		auto result_modifiers = frame.TakeResult<vector<unique_ptr<ResultModifier>>>(2);
		for (auto &result_modifier : result_modifiers) {
			select_statement->node->modifiers.push_back(std::move(result_modifier));
		}
	}
	if (select_statement->node->type == QueryNodeType::SELECT_NODE) {
		auto &select_node = select_statement->node->Cast<SelectNode>();
		if (select_node.from_table->type == TableReferenceType::SHOW_REF) {
			auto &show_ref = select_node.from_table->Cast<ShowRef>();
			if (!select_statement->node->cte_map.map.empty()) {
				throw ParserException("%s with CTE not allowed - wrap the statement in a subquery instead",
				                      EnumUtil::ToString(show_ref.show_type));
			}
			if (!select_statement->node->modifiers.empty()) {
				throw ParserException("%s with ORDER BY not allowed - wrap the statement in a subquery instead",
				                      EnumUtil::ToString(show_ref.show_type));
			}
		}
	}
	return make_uniq<TypedTransformResult<unique_ptr<SelectStatement>>>(std::move(select_statement));
}

static bool IsBareRelationSelect(const QueryNode &node) {
	if (node.type != QueryNodeType::SELECT_NODE || !node.modifiers.empty()) {
		return false;
	}
	auto &select_node = node.Cast<SelectNode>();
	if (!select_node.from_table || select_node.select_list.size() != 1) {
		return false;
	}
	if (select_node.having || select_node.qualify || !select_node.groups.group_expressions.empty() ||
	    !select_node.groups.grouping_sets.empty() ||
	    select_node.aggregate_handling != AggregateHandling::STANDARD_HANDLING) {
		return false;
	}
	return IsPlainUnqualifiedStar(*select_node.select_list[0]);
}

// a pipe clause carries its input's columns forward, so it starts out projecting all of them
static unique_ptr<SelectNode> MakeInputProjection() {
	auto result = make_uniq<SelectNode>();
	result->select_list.push_back(make_uniq<StarExpression>());
	return result;
}

unique_ptr<SelectNode> PEGTransformerFactory::TransformPipeWhereClause(PEGTransformer &transformer,
                                                                       unique_ptr<ParsedExpression> where_clause) {
	auto result = MakeInputProjection();
	result->where_clause = std::move(where_clause);
	return result;
}

unique_ptr<SelectNode>
PEGTransformerFactory::TransformPipeExtendClause(PEGTransformer &transformer,
                                                 vector<unique_ptr<ParsedExpression>> target_list) {
	auto result = MakeInputProjection();
	for (auto &expression : target_list) {
		result->select_list.push_back(std::move(expression));
	}
	return result;
}

pair<Identifier, unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformPipeSetAssignment(PEGTransformer &transformer, const Identifier &column_name,
                                                  unique_ptr<ParsedExpression> expression) {
	return make_pair(column_name, std::move(expression));
}

unique_ptr<SelectNode> PEGTransformerFactory::TransformPipeSetClause(
    PEGTransformer &transformer, vector<pair<Identifier, unique_ptr<ParsedExpression>>> pipe_set_assignment) {
	unique_ptr<SelectNode> result;
	for (auto &assignment : pipe_set_assignment) {
		auto projection = MakeInputProjection();
		projection->select_list[0]->Cast<StarExpression>().ReplaceListMutable()[assignment.first] =
		    std::move(assignment.second);
		if (result) {
			auto input = make_uniq<SelectStatement>();
			input->node = std::move(result);
			projection->from_table = make_uniq<SubqueryRef>(std::move(input));
		}
		result = std::move(projection);
	}
	return result;
}

unique_ptr<SelectNode> PEGTransformerFactory::TransformPipeDropClause(PEGTransformer &transformer,
                                                                      const vector<Identifier> &column_name) {
	auto result = MakeInputProjection();
	auto &exclude_list = result->select_list[0]->Cast<StarExpression>().ExcludeListMutable();
	for (auto &name : column_name) {
		exclude_list.insert(QualifiedColumnName(name));
	}
	return result;
}

unique_ptr<SelectNode> PEGTransformerFactory::TransformPipeJoinClause(PEGTransformer &transformer,
                                                                      unique_ptr<TableRef> join_clause) {
	auto result = MakeInputProjection();
	result->from_table = std::move(join_clause);
	return result;
}

unique_ptr<SelectNode> PEGTransformerFactory::TransformPipeOrderByClause(PEGTransformer &transformer,
                                                                         vector<OrderByNode> order_by_clause) {
	auto result = MakeInputProjection();
	auto order_modifier = make_uniq<OrderModifier>();
	order_modifier->orders = std::move(order_by_clause);
	result->modifiers.push_back(std::move(order_modifier));
	return result;
}

unique_ptr<SelectNode>
PEGTransformerFactory::TransformPipeLimitOffsetClause(PEGTransformer &transformer,
                                                      unique_ptr<ResultModifier> limit_offset) {
	auto result = MakeInputProjection();
	result->modifiers.push_back(std::move(limit_offset));
	return result;
}

static optional_ptr<JoinRef> PipeJoin(SelectNode &pipe_node) {
	if (!pipe_node.from_table || pipe_node.from_table->type != TableReferenceType::JOIN) {
		return nullptr;
	}
	return pipe_node.from_table->Cast<JoinRef>();
}

using PipeAliases = identifier_map_t<unique_ptr<ParsedExpression>>;

//! The name a select list entry exposes to later pipe clauses; empty when the binder names the entry itself
static Identifier PipeOutputName(const ParsedExpression &expr) {
	if (expr.HasAlias()) {
		return expr.GetAlias();
	}
	if (expr.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		return expr.Cast<ColumnRefExpression>().GetColumnName();
	}
	return Identifier();
}

//! A star standing for every column of its clause's input, EXCLUDE/REPLACE/RENAME included
static bool IsInputStar(const ParsedExpression &expr) {
	if (!StarExpression::IsStar(expr)) {
		return false;
	}
	auto &star = expr.Cast<StarExpression>();
	return star.RelationName().empty() && !star.Expression();
}

static optional_ptr<StarExpression> PipeInputStar(SelectNode &node) {
	if (node.select_list.empty() || !IsInputStar(*node.select_list[0])) {
		return nullptr;
	}
	return node.select_list[0]->Cast<StarExpression>();
}

static optional_idx FindPipeEntry(const SelectNode &node, const Identifier &name) {
	for (idx_t i = 0; i < node.select_list.size(); i++) {
		auto entry_name = PipeOutputName(*node.select_list[i]);
		if (!entry_name.empty() && entry_name == name) {
			return i;
		}
	}
	return optional_idx();
}

//! Only a star can hold a qualifier, so a qualified name never names an entry of its own
static optional_idx FindPipeEntry(const SelectNode &node, const QualifiedColumnName &name) {
	if (name.IsQualified()) {
		return optional_idx();
	}
	return FindPipeEntry(node, name.column);
}

static void RecordPipeAlias(PipeAliases &aliases, const Identifier &name, const ParsedExpression &expr) {
	if (name.empty()) {
		return;
	}
	auto definition = expr.Copy();
	definition->ClearAlias();
	aliases[name] = std::move(definition);
}

//! Columns a node names itself shadow the ones its FROM clause provides, which resolve on their own
static void CollectPipeAliases(SelectNode &node, PipeAliases &aliases) {
	aliases.clear();
	auto star = PipeInputStar(node);
	if (star) {
		for (auto &replaced : star->ReplaceList()) {
			RecordPipeAlias(aliases, replaced.first, *replaced.second);
		}
	}
	for (auto &entry : node.select_list) {
		RecordPipeAlias(aliases, PipeOutputName(*entry), *entry);
	}
}

static void SubstitutePipeAliases(unique_ptr<ParsedExpression> &expr, const PipeAliases &aliases) {
	if (aliases.empty()) {
		return;
	}
	if (expr->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		auto &column_ref = expr->Cast<ColumnRefExpression>();
		if (!column_ref.IsQualified()) {
			auto entry = aliases.find(column_ref.GetColumnName());
			if (entry != aliases.end()) {
				auto alias = expr->GetAlias();
				expr = entry->second->Copy();
				expr->SetAlias(std::move(alias));
				return;
			}
		}
	}
	ParsedExpressionIterator::EnumerateChildren(
	    *expr, [&](unique_ptr<ParsedExpression> &child) { SubstitutePipeAliases(child, aliases); });
}

//! An entry that took its name from the reference it substitutes keeps that name, so `|> select z` returns `z`
static void SubstitutePipeAliasesInEntry(unique_ptr<ParsedExpression> &entry, const PipeAliases &aliases) {
	auto name = PipeOutputName(*entry);
	SubstitutePipeAliases(entry, aliases);
	if (!name.empty() && PipeOutputName(*entry) != name) {
		entry->SetAlias(std::move(name));
	}
}

static bool HasModifier(const SelectNode &node, ResultModifierType type) {
	for (auto &modifier : node.modifiers) {
		if (modifier->type == type) {
			return true;
		}
	}
	return false;
}

static bool HasRowLimit(const SelectNode &node) {
	return HasModifier(node, ResultModifierType::LIMIT_MODIFIER) ||
	       HasModifier(node, ResultModifierType::LEGACY_LIMIT_PERCENT_MODIFIER);
}

//! Folding replays a clause against the accumulated node's own FROM clause, reordering it with respect to
//! everything that node already applies - only orderings that leave the result unchanged may fold
static bool PipeModifiersAllowFold(const SelectNode &target, const SelectNode &clause) {
	auto target_limit = HasRowLimit(target);
	auto target_order = HasModifier(target, ResultModifierType::ORDER_MODIFIER);
	if (clause.where_clause && target_limit) {
		return false;
	}
	if (HasModifier(clause, ResultModifierType::ORDER_MODIFIER) && (target_order || target_limit)) {
		return false;
	}
	if (HasRowLimit(clause) && target_limit) {
		return false;
	}
	if (HasModifier(clause, ResultModifierType::DISTINCT_MODIFIER) && (target_order || target_limit)) {
		return false;
	}
	auto keeps_columns = clause.select_list.size() == 1 && IsPlainUnqualifiedStar(*clause.select_list[0]);
	return keeps_columns || !HasModifier(target, ResultModifierType::DISTINCT_MODIFIER);
}

static bool PipeNameIsFoldable(SelectNode &target, const QualifiedColumnName &name) {
	return FindPipeEntry(target, name).IsValid() || PipeInputStar(target) != nullptr;
}

static bool PipeSelectListFoldable(SelectNode &target, SelectNode &clause) {
	auto clause_star = PipeInputStar(clause);
	auto target_carries_input = target.select_list.size() == 1 && IsPlainStar(*target.select_list[0]);
	for (idx_t i = clause_star ? 1 : 0; i < clause.select_list.size(); i++) {
		// a star that is not the clause's leading one expands over the input relation, which the accumulated
		// list only still stands for while it is an untouched star
		if (StarExpression::IsStar(*clause.select_list[i]) && !target_carries_input) {
			return false;
		}
	}
	if (!clause_star) {
		return true;
	}
	auto lists = (clause_star->ExcludeList().empty() ? 0 : 1) + (clause_star->ReplaceList().empty() ? 0 : 1) +
	             (clause_star->RenameList().empty() ? 0 : 1);
	// with no star to fall back on every name has to stay an entry, which two lists at once cannot promise:
	// whichever runs first can drop or rename the entry the other one still expects to find
	if (lists > 1 && !PipeInputStar(target)) {
		return false;
	}
	for (auto &excluded : clause_star->ExcludeList()) {
		if (!PipeNameIsFoldable(target, excluded)) {
			return false;
		}
	}
	for (auto &replaced : clause_star->ReplaceList()) {
		if (!PipeNameIsFoldable(target, QualifiedColumnName(replaced.first))) {
			return false;
		}
	}
	for (auto &renamed : clause_star->RenameList()) {
		if (!PipeNameIsFoldable(target, renamed.first)) {
			return false;
		}
	}
	return true;
}

static void DropPipeColumn(SelectNode &target, PipeAliases &aliases, const QualifiedColumnName &name) {
	aliases.erase(name.column);
	auto entry = FindPipeEntry(target, name);
	if (entry.IsValid()) {
		target.select_list.erase(target.select_list.begin() + NumericCast<int64_t>(entry.GetIndex()));
		return;
	}
	auto target_star = PipeInputStar(target);
	target_star->ReplaceListMutable().erase(name.column);
	target_star->RenameListMutable().erase(name);
	target_star->ExcludeListMutable().insert(name);
}

static void ReplacePipeColumn(SelectNode &target, PipeAliases &aliases, const Identifier &name,
                              unique_ptr<ParsedExpression> definition) {
	RecordPipeAlias(aliases, name, *definition);
	auto entry = FindPipeEntry(target, name);
	if (entry.IsValid()) {
		definition->SetAlias(name);
		target.select_list[entry.GetIndex()] = std::move(definition);
		return;
	}
	PipeInputStar(target)->ReplaceListMutable()[name] = std::move(definition);
}

static void RenamePipeColumn(SelectNode &target, PipeAliases &aliases, const QualifiedColumnName &name,
                             const Identifier &renamed) {
	auto entry = FindPipeEntry(target, name);
	if (entry.IsValid()) {
		target.select_list[entry.GetIndex()]->SetAlias(renamed);
	} else {
		PipeInputStar(target)->RenameListMutable()[name] = renamed;
	}
	auto definition = aliases.find(name.column);
	if (definition != aliases.end()) {
		aliases[renamed] = std::move(definition->second);
		aliases.erase(name.column);
	}
}

//! The clause's own star stands for the accumulated list, so it is applied to that list rather than emitted
static void FoldPipeSelectList(SelectNode &target, SelectNode &clause, PipeAliases &aliases) {
	auto clause_star = PipeInputStar(clause);
	if (!clause_star) {
		for (auto &entry : clause.select_list) {
			SubstitutePipeAliasesInEntry(entry, aliases);
		}
		target.select_list = std::move(clause.select_list);
		CollectPipeAliases(target, aliases);
		return;
	}
	for (auto &replaced : clause_star->ReplaceListMutable()) {
		SubstitutePipeAliases(replaced.second, aliases);
		ReplacePipeColumn(target, aliases, replaced.first, std::move(replaced.second));
	}
	for (auto &renamed : clause_star->RenameList()) {
		RenamePipeColumn(target, aliases, renamed.first, renamed.second);
	}
	// EXCLUDE last: a column the clause both replaces and excludes is excluded
	for (auto &excluded : clause_star->ExcludeList()) {
		DropPipeColumn(target, aliases, excluded);
	}
	for (idx_t i = 1; i < clause.select_list.size(); i++) {
		auto &entry = clause.select_list[i];
		SubstitutePipeAliasesInEntry(entry, aliases);
		RecordPipeAlias(aliases, PipeOutputName(*entry), *entry);
		target.select_list.push_back(std::move(entry));
	}
}

static void FoldPipeWhereAndModifiers(SelectNode &target, SelectNode &clause, const PipeAliases &aliases) {
	if (clause.where_clause) {
		SubstitutePipeAliases(clause.where_clause, aliases);
		if (target.where_clause) {
			target.where_clause = make_uniq<ConjunctionExpression>(
			    ExpressionType::CONJUNCTION_AND, std::move(target.where_clause), std::move(clause.where_clause));
		} else {
			target.where_clause = std::move(clause.where_clause);
		}
	}
	ParsedExpressionIterator::EnumerateQueryNodeModifiers(
	    clause, [&](unique_ptr<ParsedExpression> &child) { SubstitutePipeAliases(child, aliases); });
	for (auto &modifier : clause.modifiers) {
		target.modifiers.push_back(std::move(modifier));
	}
}

static bool TryFoldPipeClause(SelectNode &target, SelectNode &clause, PipeAliases &aliases) {
	if (!PipeModifiersAllowFold(target, clause) || !PipeSelectListFoldable(target, clause)) {
		return false;
	}
	FoldPipeWhereAndModifiers(target, clause, aliases);
	FoldPipeSelectList(target, clause, aliases);
	return true;
}

// a SET clause lowers to one projection per assignment, stacked so that each assignment sees the previous
// one's result; the chain replays that stack innermost first
static void FlattenPipeClause(unique_ptr<SelectNode> pipe_node, vector<unique_ptr<SelectNode>> &result) {
	if (pipe_node->from_table && pipe_node->from_table->type == TableReferenceType::SUBQUERY) {
		auto input = std::move(pipe_node->from_table->Cast<SubqueryRef>().subquery->node);
		pipe_node->from_table = nullptr;
		FlattenPipeClause(unique_ptr_cast<QueryNode, SelectNode>(std::move(input)), result);
	}
	result.push_back(std::move(pipe_node));
}

// splicing an input's relation into a JOIN drops the node that held it, so everything that node applies
// must already be part of the relation itself
static bool IsSpliceableRelationSelect(const QueryNode &node) {
	if (!IsBareRelationSelect(node) || !node.cte_map.map.empty()) {
		return false;
	}
	auto &select_node = node.Cast<SelectNode>();
	return !select_node.where_clause && !select_node.sample;
}

unique_ptr<SelectStatement>
PEGTransformerFactory::TransformPipeOperatorChain(PEGTransformer &transformer,
                                                  unique_ptr<SelectStatement> select_set_op_chain,
                                                  optional<vector<unique_ptr<SelectNode>>> pipe_operator_clause) {
	auto select = std::move(select_set_op_chain);
	if (!pipe_operator_clause) {
		return select;
	}
	PipeAliases aliases;
	// duckdb resolves `tbl.col` only while `tbl` sits in the FROM clause of the node being bound, so clauses fold
	// into one node instead of stacking subqueries, each of which would hide its input behind one unnamed binding
	auto foldable = IsBareRelationSelect(*select->node);
	for (auto &pipe_node : *pipe_operator_clause) {
		vector<unique_ptr<SelectNode>> clauses;
		FlattenPipeClause(std::move(pipe_node), clauses);
		for (auto &clause : clauses) {
			auto pipe_join = PipeJoin(*clause);
			if (!pipe_join && foldable && TryFoldPipeClause(select->node->Cast<SelectNode>(), *clause, aliases)) {
				continue;
			}
			if (!pipe_join) {
				clause->from_table = make_uniq<SubqueryRef>(std::move(select));
			} else if (IsSpliceableRelationSelect(*select->node)) {
				pipe_join->left = std::move(select->node->Cast<SelectNode>().from_table);
			} else {
				pipe_join->left = make_uniq<SubqueryRef>(std::move(select));
			}
			select = make_uniq<SelectStatement>();
			select->node = std::move(clause);
			CollectPipeAliases(select->node->Cast<SelectNode>(), aliases);
			foldable = true;
		}
	}
	return select;
}

unique_ptr<SelectStatement> PEGTransformerFactory::TransformSelectSetOpChain(
    PEGTransformer &transformer, unique_ptr<SelectStatement> intersect_chain,
    optional<vector<pair<unique_ptr<SetOperationNode>, unique_ptr<SelectStatement>>>> select_set_op_chain_tail) {
	auto select = std::move(intersect_chain);
	if (!select_set_op_chain_tail) {
		return select;
	}
	for (auto &tail : *select_set_op_chain_tail) {
		auto setop_result = std::move(tail.first);
		auto right_select = std::move(tail.second);
		if (select->node->type == QueryNodeType::SET_OPERATION_NODE && select->node->modifiers.empty() &&
		    select->node->cte_map.map.empty()) {
			auto &existing = select->node->Cast<SetOperationNode>();
			if (existing.setop_type == setop_result->setop_type && existing.setop_all == setop_result->setop_all &&
			    (existing.setop_type == SetOperationType::UNION ||
			     existing.setop_type == SetOperationType::UNION_BY_NAME)) {
				existing.children.push_back(std::move(right_select->node));
				continue;
			}
		}
		setop_result->children.push_back(std::move(select->node));
		setop_result->children.push_back(std::move(right_select->node));
		select->node = std::move(setop_result);
	}
	return select;
}

pair<unique_ptr<SetOperationNode>, unique_ptr<SelectStatement>>
PEGTransformerFactory::TransformSelectSetOpChainTail(PEGTransformer &transformer,
                                                     unique_ptr<SetOperationNode> setop_clause,
                                                     unique_ptr<SelectStatement> intersect_chain) {
	return make_pair(std::move(setop_clause), std::move(intersect_chain));
}

unique_ptr<SelectStatement> PEGTransformerFactory::TransformIntersectChain(
    PEGTransformer &transformer, unique_ptr<SelectStatement> select_atom,
    optional<vector<pair<unique_ptr<SetOperationNode>, unique_ptr<SelectStatement>>>> intersect_chain_tail) {
	auto select = std::move(select_atom);
	if (!intersect_chain_tail) {
		return select;
	}
	for (auto &tail : *intersect_chain_tail) {
		auto intersect_node = std::move(tail.first);
		auto right_select = std::move(tail.second);
		intersect_node->children.push_back(std::move(select->node));
		intersect_node->children.push_back(std::move(right_select->node));
		select->node = std::move(intersect_node);
	}
	return select;
}

pair<unique_ptr<SetOperationNode>, unique_ptr<SelectStatement>>
PEGTransformerFactory::TransformIntersectChainTail(PEGTransformer &transformer,
                                                   unique_ptr<SetOperationNode> set_intersect_clause,
                                                   unique_ptr<SelectStatement> select_atom) {
	return make_pair(std::move(set_intersect_clause), std::move(select_atom));
}

unique_ptr<SetOperationNode> PEGTransformerFactory::TransformSetIntersectClause(PEGTransformer &transformer,
                                                                                const optional<bool> &distinct_or_all) {
	auto result = make_uniq<SetOperationNode>();
	result->setop_type = SetOperationType::INTERSECT;
	if (distinct_or_all) {
		result->setop_all = !*distinct_or_all;
	}
	return result;
}

unique_ptr<SetOperationNode> PEGTransformerFactory::TransformSetopClause(PEGTransformer &transformer,
                                                                         const SetOperationType &setop_type,
                                                                         const optional<bool> &distinct_or_all,
                                                                         const bool &has_result) {
	auto result = make_uniq<SetOperationNode>();
	result->setop_type = setop_type;
	if (distinct_or_all) {
		result->setop_all = !*distinct_or_all;
	}
	if (has_result) {
		if (result->setop_type == SetOperationType::UNION) {
			result->setop_type = SetOperationType::UNION_BY_NAME;
		} else {
			throw ParserException("Invalid combination of %s and BY NAME", EnumUtil::ToString(result->setop_type));
		}
	}
	return result;
}

SetOperationType PEGTransformerFactory::TransformSetopUnion(PEGTransformer &transformer) {
	return SetOperationType::UNION;
}

SetOperationType PEGTransformerFactory::TransformSetopExcept(PEGTransformer &transformer) {
	return SetOperationType::EXCEPT;
}

bool PEGTransformerFactory::TransformLateral(PEGTransformer &transformer) {
	return true;
}

bool PEGTransformerFactory::TransformWithOrdinality(PEGTransformer &transformer) {
	return true;
}

unique_ptr<SelectStatement> PEGTransformerFactory::TransformSimpleSelect(PEGTransformer &transformer,
                                                                         ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	auto &opt_window_clause = list_pr.Child<OptionalParseResult>(4);
	if (opt_window_clause.HasResult()) {
		transformer.Transform<vector<unique_ptr<ParsedExpression>>>(opt_window_clause.GetResult());
	}
	auto select_node = transformer.Transform<unique_ptr<SelectNode>>(list_pr.Child<ListParseResult>(0));
	transformer.TransformOptional<unique_ptr<ParsedExpression>>(list_pr, 1, select_node->where_clause);
	auto &group_opt = list_pr.Child<OptionalParseResult>(2);
	if (group_opt.HasResult()) {
		AssignGroupByNode(*select_node, transformer.Transform<GroupByNode>(group_opt.GetResult()));
	}
	transformer.TransformOptional<unique_ptr<ParsedExpression>>(list_pr, 3, select_node->having);
	transformer.TransformOptional<unique_ptr<ParsedExpression>>(list_pr, 5, select_node->qualify);
	transformer.TransformOptional<unique_ptr<SampleOptions>>(list_pr, 6, select_node->sample);
	auto select_statement = make_uniq<SelectStatement>();
	select_statement->node = std::move(select_node);
	transformer.window_clauses.clear();
	return select_statement;
}

static void RegisterWindowClause(PEGTransformer &transformer, const Identifier &window_name,
                                 WindowExpression &window_function) {
	auto it = transformer.window_clauses.find(window_name);
	if (it != transformer.window_clauses.end()) {
		throw ParserException("window \"%s\" is already defined", window_name.GetIdentifierName());
	}
	transformer.window_clauses[window_name] =
	    unique_ptr_cast<ParsedExpression, WindowExpression>(window_function.Copy());
}

static void PushSimpleSelectRemainder(TransformStack &stack, TransformStackFrame &frame) {
	auto &list_pr = frame.parse_result.Cast<ListParseResult>();
	auto &sample_clause_opt = list_pr.Child<OptionalParseResult>(6);
	if (sample_clause_opt.HasResult()) {
		stack.PushFrame(sample_clause_opt.GetResult(), PEGTransformerFactory::GetTrampolineOps("SampleClause"),
		                TransformFrameResultTarget(frame.frame_index, 6));
	}
	auto &qualify_clause_opt = list_pr.Child<OptionalParseResult>(5);
	if (qualify_clause_opt.HasResult()) {
		stack.PushFrame(qualify_clause_opt.GetResult(), PEGTransformerFactory::GetTrampolineOps("QualifyClause"),
		                TransformFrameResultTarget(frame.frame_index, 5));
	}
	auto &having_clause_opt = list_pr.Child<OptionalParseResult>(3);
	if (having_clause_opt.HasResult()) {
		stack.PushFrame(having_clause_opt.GetResult(), PEGTransformerFactory::GetTrampolineOps("HavingClause"),
		                TransformFrameResultTarget(frame.frame_index, 3));
	}
	auto &group_by_clause_opt = list_pr.Child<OptionalParseResult>(2);
	if (group_by_clause_opt.HasResult()) {
		stack.PushFrame(group_by_clause_opt.GetResult(), PEGTransformerFactory::GetTrampolineOps("GroupByClause"),
		                TransformFrameResultTarget(frame.frame_index, 2));
	}
	auto &where_clause_opt = list_pr.Child<OptionalParseResult>(1);
	if (where_clause_opt.HasResult()) {
		stack.PushFrame(where_clause_opt.GetResult(), PEGTransformerFactory::GetTrampolineOps("WhereClause"),
		                TransformFrameResultTarget(frame.frame_index, 1));
	}
	stack.PushFrame(list_pr.GetChild(0), PEGTransformerFactory::GetTrampolineOps("SelectFrom"),
	                TransformFrameResultTarget(frame.frame_index, 0));
}

void PEGTransformerFactory::InitializeSimpleSelectTrampoline(PEGTransformer &transformer, TransformStack &stack,
                                                             TransformStackFrame &frame) {
	auto &list_pr = frame.parse_result.Cast<ListParseResult>();
	frame.ReserveChildSlots(7);
	auto &window_clause_opt = list_pr.Child<OptionalParseResult>(4);
	if (window_clause_opt.HasResult()) {
		frame.manual_state = 0;
		stack.PushFrame(window_clause_opt.GetResult(), PEGTransformerFactory::GetTrampolineOps("WindowClause"),
		                TransformFrameResultTarget(frame.frame_index, 4));
		return;
	}
	frame.manual_state = 1;
	PushSimpleSelectRemainder(stack, frame);
}

unique_ptr<TransformResultValue> PEGTransformerFactory::FinalizeSimpleSelectTrampoline(PEGTransformer &transformer,
                                                                                       TransformStack &stack,
                                                                                       TransformStackFrame &frame) {
	if (frame.manual_state == 0) {
		frame.TakeResult<vector<unique_ptr<ParsedExpression>>>(4);
		frame.manual_state = 1;
		PushSimpleSelectRemainder(stack, frame);
		return nullptr;
	}

	auto select_node = frame.TakeResult<unique_ptr<SelectNode>>(0);
	if (frame.child_results[1]) {
		select_node->where_clause = frame.TakeResult<unique_ptr<ParsedExpression>>(1);
	}
	if (frame.child_results[2]) {
		AssignGroupByNode(*select_node, frame.TakeResult<GroupByNode>(2));
	}
	if (frame.child_results[3]) {
		select_node->having = frame.TakeResult<unique_ptr<ParsedExpression>>(3);
	}
	if (frame.child_results[5]) {
		select_node->qualify = frame.TakeResult<unique_ptr<ParsedExpression>>(5);
	}
	if (frame.child_results[6]) {
		select_node->sample = frame.TakeResult<unique_ptr<SampleOptions>>(6);
	}
	auto select_statement = make_uniq<SelectStatement>();
	select_statement->node = std::move(select_node);
	transformer.window_clauses.clear();
	return make_uniq<TypedTransformResult<unique_ptr<SelectStatement>>>(std::move(select_statement));
}

FunctionArgument PEGTransformerFactory::TransformNamedFunctionArgument(PEGTransformer &transformer,
                                                                       MacroParameter named_parameter) {
	named_parameter.expression->SetAlias(named_parameter.name);
	return FunctionArgument(named_parameter.name, std::move(named_parameter.expression));
}

FunctionArgument PEGTransformerFactory::TransformPositionalFunctionArgument(PEGTransformer &transformer,
                                                                            unique_ptr<ParsedExpression> expression) {
	return FunctionArgument(std::move(expression));
}

MacroParameter PEGTransformerFactory::TransformNamedParameter(PEGTransformer &transformer,
                                                              const Identifier &reserved_identifier,
                                                              const optional<LogicalType> &type,
                                                              unique_ptr<ParsedExpression> expression) {
	MacroParameter parameter;
	parameter.expression = std::move(expression);
	parameter.name = reserved_identifier;
	parameter.is_default = true;
	if (type) {
		parameter.type = *type;
	}
	return parameter;
}

unique_ptr<BaseTableRef> PEGTransformerFactory::TransformUnqualifiedBaseTableName(PEGTransformer &transformer,
                                                                                  const Identifier &table_name) {
	const auto description = TableDescription(QualifiedName(table_name));
	return make_uniq<BaseTableRef>(description);
}

Identifier PEGTransformerFactory::TransformSchemaQualification(PEGTransformer &transformer,
                                                               const Identifier &schema_name) {
	return schema_name;
}

Identifier PEGTransformerFactory::TransformReservedSchemaQualification(PEGTransformer &transformer,
                                                                       const Identifier &reserved_schema_name) {
	return reserved_schema_name;
}

Identifier PEGTransformerFactory::TransformCatalogQualification(PEGTransformer &transformer,
                                                                const Identifier &catalog_name) {
	return catalog_name;
}

QualifiedName PEGTransformerFactory::TransformCatalogReservedSchemaIdentifier(
    PEGTransformer &transformer, const Identifier &catalog_qualification,
    const Identifier &reserved_schema_qualification, const Identifier &reserved_identifier_or_string_literal) {
	QualifiedName result(catalog_qualification, reserved_schema_qualification, reserved_identifier_or_string_literal);
	return result;
}

QualifiedName PEGTransformerFactory::TransformSchemaReservedIdentifierOrStringLiteral(
    PEGTransformer &transformer, const Identifier &schema_qualification,
    const Identifier &reserved_identifier_or_string_literal) {
	QualifiedName result({schema_qualification}, reserved_identifier_or_string_literal);
	return result;
}

static bool IsConditionlessJoin(const JoinRef &join) {
	if (join.condition || !join.using_columns.empty()) {
		return false;
	}
	if (join.ref_type != JoinRefType::CROSS && join.ref_type != JoinRefType::POSITIONAL &&
	    join.ref_type != JoinRefType::NATURAL) {
		return false;
	}
	return true;
}

static unique_ptr<TableRef> ReassociateJoins(unique_ptr<TableRef> root) {
	// Left-rotate while the current node is a conditionless join and its right child is a join.
	// This converts right-associative join trees (from PEG grammar) to left-associative.
	while (root->type == TableReferenceType::JOIN) {
		auto &current = root->Cast<JoinRef>();
		if (!IsConditionlessJoin(current) || !current.right || current.right->type != TableReferenceType::JOIN) {
			break;
		}
		// Left rotation:
		//   current(left=A, right=inner(left=B, right=C))
		//   => inner(left=current(left=A, right=B), right=C)
		auto inner = std::move(current.right);
		auto &inner_join = inner->Cast<JoinRef>();
		current.right = std::move(inner_join.left);
		inner_join.left = std::move(root);
		root = std::move(inner);
	}
	return root;
}

//! Check whether the RHS TableRef of a JoinOrPivot parse result has its own JoinOrPivot* entries.
//! This distinguishes PEG right-recursion (has entries) from parenthesized joins (no entries).
//! Navigation: JoinOrPivot → Choice → JoinClause → Choice → JoinWithoutOnClause → child(2)=TableRef → child(1)=Optional
static bool RHSTableRefHasJoinOrPivot(ParseResult &join_or_pivot_pr) {
	auto &jop_list = join_or_pivot_pr.Cast<ListParseResult>();
	auto &jop_choice = jop_list.Child<ChoiceParseResult>(0);
	auto &join_clause = jop_choice.GetResult().Cast<ListParseResult>();
	auto &jc_choice = join_clause.Child<ChoiceParseResult>(0);
	auto &join_impl = jc_choice.GetResult().Cast<ListParseResult>();
	// For JoinWithoutOnClause the TableRef is at index 2
	auto &table_ref = join_impl.Child<ListParseResult>(2);
	return table_ref.Child<OptionalParseResult>(1).HasResult();
}

unique_ptr<TableRef> PEGTransformerFactory::TransformTableRef(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	auto inner_table_ref = transformer.Transform<unique_ptr<TableRef>>(list_pr.Child<ListParseResult>(0));
	auto &join_or_pivot_opt = list_pr.Child<OptionalParseResult>(1);
	if (!join_or_pivot_opt.HasResult()) {
		return inner_table_ref;
	}
	auto &repeat_join_or_pivot = join_or_pivot_opt.GetResult().Cast<RepeatParseResult>();
	for (auto join_or_pivot : repeat_join_or_pivot.GetChildren()) {
		auto transform_join_or_pivot = transformer.Transform<unique_ptr<TableRef>>(join_or_pivot);
		if (transform_join_or_pivot->type == TableReferenceType::JOIN) {
			auto &join_ref = transform_join_or_pivot->Cast<JoinRef>();
			join_ref.left = std::move(inner_table_ref);
			if (IsConditionlessJoin(join_ref) && RHSTableRefHasJoinOrPivot(join_or_pivot)) {
				inner_table_ref = ReassociateJoins(std::move(transform_join_or_pivot));
			} else {
				inner_table_ref = std::move(transform_join_or_pivot);
			}
		} else if (transform_join_or_pivot->type == TableReferenceType::PIVOT) {
			auto &pivot_ref = transform_join_or_pivot->Cast<PivotRef>();
			pivot_ref.source = std::move(inner_table_ref);
			inner_table_ref = std::move(transform_join_or_pivot);
		} else {
			throw NotImplementedException("Unsupported TableRef type encountered: %s",
			                              EnumUtil::ToString(transform_join_or_pivot->type));
		}
	}
	return inner_table_ref;
}

void PEGTransformerFactory::InitializeTableRefTrampoline(PEGTransformer &transformer, TransformStack &stack,
                                                         TransformStackFrame &frame) {
	auto &list_pr = frame.parse_result.Cast<ListParseResult>();
	auto &join_or_pivot_opt = list_pr.Child<OptionalParseResult>(1);
	idx_t join_count = 0;
	if (join_or_pivot_opt.HasResult()) {
		join_count = join_or_pivot_opt.GetResult().Cast<RepeatParseResult>().GetChildren().size();
	}
	frame.ReserveChildSlots(1 + join_count);
	if (join_count > 0) {
		auto repeat_children = join_or_pivot_opt.GetResult().Cast<RepeatParseResult>().GetChildren();
		for (idx_t i = repeat_children.size(); i > 0; i--) {
			auto child_idx = i - 1;
			stack.PushFrame(repeat_children[child_idx].get(), PEGTransformerFactory::GetTrampolineOps("JoinOrPivot"),
			                TransformFrameResultTarget(frame.frame_index, 1 + child_idx));
		}
	}
	stack.PushFrame(list_pr.GetChild(0), PEGTransformerFactory::GetTrampolineOps("InnerTableRef"),
	                TransformFrameResultTarget(frame.frame_index, 0));
}

unique_ptr<TransformResultValue> PEGTransformerFactory::FinalizeTableRefTrampoline(PEGTransformer &transformer,
                                                                                   TransformStack &stack,
                                                                                   TransformStackFrame &frame) {
	auto &list_pr = frame.parse_result.Cast<ListParseResult>();
	auto inner_table_ref = frame.TakeResult<unique_ptr<TableRef>>(0);
	auto &join_or_pivot_opt = list_pr.Child<OptionalParseResult>(1);
	if (!join_or_pivot_opt.HasResult()) {
		return make_uniq<TypedTransformResult<unique_ptr<TableRef>>>(std::move(inner_table_ref));
	}
	auto &repeat_join_or_pivot = join_or_pivot_opt.GetResult().Cast<RepeatParseResult>();
	auto repeat_children = repeat_join_or_pivot.GetChildren();
	for (idx_t i = 0; i < repeat_children.size(); i++) {
		auto transform_join_or_pivot = frame.TakeResult<unique_ptr<TableRef>>(1 + i);
		if (transform_join_or_pivot->type == TableReferenceType::JOIN) {
			auto &join_ref = transform_join_or_pivot->Cast<JoinRef>();
			join_ref.left = std::move(inner_table_ref);
			if (IsConditionlessJoin(join_ref) && RHSTableRefHasJoinOrPivot(repeat_children[i].get())) {
				inner_table_ref = ReassociateJoins(std::move(transform_join_or_pivot));
			} else {
				inner_table_ref = std::move(transform_join_or_pivot);
			}
		} else if (transform_join_or_pivot->type == TableReferenceType::PIVOT) {
			auto &pivot_ref = transform_join_or_pivot->Cast<PivotRef>();
			pivot_ref.source = std::move(inner_table_ref);
			inner_table_ref = std::move(transform_join_or_pivot);
		} else {
			throw NotImplementedException("Unsupported TableRef type encountered: %s",
			                              EnumUtil::ToString(transform_join_or_pivot->type));
		}
	}
	return make_uniq<TypedTransformResult<unique_ptr<TableRef>>>(std::move(inner_table_ref));
}

unique_ptr<TableRef> PEGTransformerFactory::TransformTableUnpivotClauseBody(PEGTransformer &transformer,
                                                                            const vector<string> &unpivot_header,
                                                                            vector<PivotColumn> unpivot_value_list) {
	auto result = make_uniq<PivotRef>();
	result->unpivot_names = StringsToIdentifiers(unpivot_header);
	result->pivots = std::move(unpivot_value_list);
	if (result->pivots.size() > 1) {
		throw ParserException("UNPIVOT requires a single pivot element");
	}
	return std::move(result);
}

unique_ptr<TableRef> PEGTransformerFactory::TransformTableUnpivotClause(PEGTransformer &transformer,
                                                                        const optional<bool> &include_or_exclude_nulls,
                                                                        unique_ptr<TableRef> table_unpivot_clause_body,
                                                                        const optional<TableAlias> &table_alias) {
	auto &result = table_unpivot_clause_body->Cast<PivotRef>();
	result.include_nulls = include_or_exclude_nulls.value_or(false);
	if (table_alias) {
		result.alias = table_alias->name;
		result.column_name_alias = table_alias->column_name_alias;
	}
	return table_unpivot_clause_body;
}

void PEGTransformerFactory::GetValueFromExpression(unique_ptr<ParsedExpression> &expr, vector<Value> &result) {
	if (expr->GetExpressionClass() == ExpressionClass::CONSTANT) {
		auto &const_expr = expr->Cast<ConstantExpression>();
		result.push_back(const_expr.GetValue());
	} else if (expr->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		auto &col_ref_expr = expr->Cast<ColumnRefExpression>();
		for (auto &col : col_ref_expr.ColumnNames()) {
			result.push_back(Value(col));
		}
	} else if (expr->GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &func_expr = expr->Cast<FunctionExpression>();
		if (func_expr.FunctionName() == "row") {
			for (auto &col : func_expr.GetArgumentsMutable()) {
				GetValueFromExpression(col.GetExpressionMutable(), result);
			}
		}
	}
}

static bool ExpressionIsTuple(const ParsedExpression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::FUNCTION) {
		return false;
	}
	return expr.Cast<FunctionExpression>().FunctionName() == "row";
}

bool PEGTransformerFactory::TransformPivotInList(unique_ptr<ParsedExpression> &expr, PivotColumnEntry &entry,
                                                 idx_t depth) {
	auto initial_size = entry.values.size();
	switch (expr->GetExpressionType()) {
	case ExpressionType::COLUMN_REF: {
		auto &colref = expr->Cast<ColumnRefExpression>();
		if (colref.IsQualified()) {
			throw ParserException(expr->GetQueryLocation(), "PIVOT IN list cannot contain qualified column references");
		}
		entry.values.emplace_back(colref.GetColumnName());
		return true;
	}
	case ExpressionType::FUNCTION: {
		auto &function = expr->Cast<FunctionExpression>();
		if (function.FunctionName() != "row") {
			return false;
		}
		// only the outermost parentheses hold one value per pivot column - a nested tuple is a struct literal,
		// so the entry stays an expression and the struct is folded into a single value at bind time
		if (depth > 0) {
			return false;
		}
		for (auto &child : function.GetArgumentsMutable()) {
			if (!TransformPivotInList(child.GetExpressionMutable(), entry, depth + 1)) {
				entry.values.resize(initial_size);
				return false;
			}
		}
		return true;
	}
	default: {
		Value val;
		if (!ConstructConstantFromExpression(*expr, val)) {
			return false;
		}
		entry.values.push_back(std::move(val));
		return true;
	}
	}
}

static bool PivotEntryIsTuple(const PivotColumnEntry &entry) {
	if (entry.values.size() > 1) {
		return true;
	}
	return entry.expr && ExpressionIsTuple(*entry.expr);
}

static bool HasTupleEntries(const PivotColumn &column) {
	for (auto &entry : column.entries) {
		if (PivotEntryIsTuple(entry)) {
			return true;
		}
	}
	return false;
}

static void AddPivotExpressions(PivotColumn &column, unique_ptr<ParsedExpression> pivot_header) {
	// Unpack row() only when IN list entries are tuples (multi-value).
	// For scalar IN entries like IN ('xx'), keep row() as a single compound expression
	// so pivot_expressions.size() matches entry.values.size() (both 1).
	if (!ExpressionIsTuple(*pivot_header) || !HasTupleEntries(column)) {
		column.pivot_expressions.push_back(std::move(pivot_header));
		return;
	}
	for (auto &child : pivot_header->Cast<FunctionExpression>().GetArgumentsMutable()) {
		column.pivot_expressions.emplace_back(std::move(child.GetExpressionMutable()));
	}
}

// A tuple goes back to an expression rather than a struct Value because the pivot filter compares stringified
// pivot values: only an expression is folded through the client context, where the Spark struct rendering
// lives - Value::DefaultCastAs never sees it.
static unique_ptr<ParsedExpression> RenderTupleAsStruct(unique_ptr<ParsedExpression> tuple) {
	return make_uniq<CastExpression>(LogicalType::VARCHAR, std::move(tuple));
}

// Spark reads a parenthesised list of pivot values as a struct literal. A single pivot column therefore takes
// the whole tuple as one value, and a multi-column one takes the outermost tuple as its value list while every
// tuple nested in it is one struct value.
static void ReadTupleValuesAsStructs(PivotColumn &column) {
	const auto single_pivot_expression = column.pivot_expressions.size() == 1;
	for (auto &entry : column.entries) {
		if (single_pivot_expression) {
			if (entry.values.size() > 1) {
				vector<unique_ptr<ParsedExpression>> fields;
				for (auto &value : entry.values) {
					fields.push_back(make_uniq<ConstantExpression>(std::move(value)));
				}
				entry.values.clear();
				entry.expr = RenderTupleAsStruct(make_uniq<FunctionExpression>("row", std::move(fields)));
			} else if (entry.expr && ExpressionIsTuple(*entry.expr)) {
				entry.expr = RenderTupleAsStruct(std::move(entry.expr));
			}
			continue;
		}
		if (!entry.expr || !ExpressionIsTuple(*entry.expr)) {
			continue;
		}
		for (auto &child : entry.expr->Cast<FunctionExpression>().GetArgumentsMutable()) {
			auto &child_expr = child.GetExpressionMutable();
			if (ExpressionIsTuple(*child_expr)) {
				child_expr = RenderTupleAsStruct(std::move(child_expr));
			}
		}
	}
}

unique_ptr<TableRef> PEGTransformerFactory::TransformTablePivotClauseBody(
    PEGTransformer &transformer, vector<unique_ptr<ParsedExpression>> target_list, vector<PivotColumn> pivot_value_list,
    const optional<vector<string>> &pivot_group_by_list) {
	auto result = make_uniq<PivotRef>();
	result->aggregates = std::move(target_list);
	result->pivots = std::move(pivot_value_list);
	if (pivot_group_by_list) {
		result->groups = StringsToIdentifiers(*pivot_group_by_list);
	}
	return std::move(result);
}

unique_ptr<TableRef> PEGTransformerFactory::TransformTablePivotClause(PEGTransformer &transformer,
                                                                      unique_ptr<TableRef> table_pivot_clause_body,
                                                                      const optional<TableAlias> &table_alias) {
	auto &result = table_pivot_clause_body->Cast<PivotRef>();
	if (table_alias) {
		result.alias = table_alias->name;
		result.column_name_alias = table_alias->column_name_alias;
	}
	return table_pivot_clause_body;
}

PivotColumn PEGTransformerFactory::TransformPivotEnumTarget(PEGTransformer &transformer, const Identifier &identifier) {
	PivotColumn result;
	result.pivot_enum = identifier;
	return result;
}

PivotColumn PEGTransformerFactory::TransformPivotListTarget(PEGTransformer &transformer,
                                                            vector<PivotColumnEntry> pivot_target_list) {
	PivotColumn result;
	result.entries = std::move(pivot_target_list);
	return result;
}

PivotColumn PEGTransformerFactory::TransformPivotValueList(PEGTransformer &transformer,
                                                           unique_ptr<ParsedExpression> pivot_header,
                                                           PivotColumn pivot_value_target) {
	auto result = std::move(pivot_value_target);
	AddPivotExpressions(result, std::move(pivot_header));
	ReadTupleValuesAsStructs(result);
	return result;
}

unique_ptr<TableRef> PEGTransformerFactory::TransformRegularJoinClause(PEGTransformer &transformer,
                                                                       const optional<bool> &asof,
                                                                       const optional<JoinType> &join_type,
                                                                       unique_ptr<TableRef> table_ref,
                                                                       JoinQualifier join_qualifier) {
	auto result = make_uniq<JoinRef>();
	if (asof.value_or(false)) {
		result->ref_type = JoinRefType::ASOF;
	}
	result->type = join_type.value_or(JoinType::INNER);
	result->right = std::move(table_ref);
	if (join_qualifier.on_clause) {
		result->condition = std::move(join_qualifier.on_clause);
	} else if (!join_qualifier.using_columns.empty()) {
		result->using_columns = std::move(join_qualifier.using_columns);
	} else {
		throw InternalException("Invalid join qualifier found.");
	}
	return std::move(result);
}

unique_ptr<TableRef> PEGTransformerFactory::TransformUnqualifiedJoinClause(PEGTransformer &transformer,
                                                                           const optional<JoinType> &join_type,
                                                                           unique_ptr<TableRef> inner_table_ref) {
	// Spark leaves the condition unset when a join carries no ON/USING, pairing every left row with
	// every right row; an explicit TRUE expresses that for every join type.
	auto result = make_uniq<JoinRef>();
	result->type = join_type.value_or(JoinType::INNER);
	result->right = std::move(inner_table_ref);
	result->condition = make_uniq<ConstantExpression>(Value::BOOLEAN(true));
	return std::move(result);
}

unique_ptr<TableRef> PEGTransformerFactory::TransformJoinByClause(PEGTransformer &transformer, const string &col_label,
                                                                  unique_ptr<TableRef> table_ref,
                                                                  JoinQualifier join_qualifier) {
	auto result = make_uniq<JoinRef>();
	// resolve the join type name against the JoinType enum (case-insensitive); accept an optional `_join` suffix,
	// so e.g. `mark` and `mark_join` are equivalent. EnumUtil::FromString throws on an unknown name.
	auto type_name = col_label;
	if (StringUtil::EndsWith(StringUtil::Lower(type_name), "_join")) {
		type_name = type_name.substr(0, type_name.size() - 5);
	}
	result->type = EnumUtil::FromString<JoinType>(type_name);
	if (result->type == JoinType::INVALID) {
		throw ParserException("\"%s\" is not a valid join type for JOIN BY", col_label);
	}
	result->right = std::move(table_ref);
	if (join_qualifier.on_clause) {
		result->condition = std::move(join_qualifier.on_clause);
	} else if (!join_qualifier.using_columns.empty()) {
		result->using_columns = std::move(join_qualifier.using_columns);
	} else {
		throw InternalException("Invalid join qualifier found.");
	}
	return std::move(result);
}

unique_ptr<TableRef> PEGTransformerFactory::TransformLateralJoinClause(PEGTransformer &transformer,
                                                                       unique_ptr<TableRef> subquery_reference,
                                                                       const optional<TableAlias> &table_alias) {
	// Spark's `JOIN LATERAL (subquery)` is an unconditioned inner lateral join (correlation is
	// auto-detected by the binder). Represent it as an INNER JOIN with an explicit TRUE condition.
	if (table_alias) {
		subquery_reference->alias = table_alias->name;
		subquery_reference->column_name_alias = table_alias->column_name_alias;
	}
	RewriteOuterQualifiedStars(*subquery_reference);
	auto result = make_uniq<JoinRef>();
	result->type = JoinType::INNER;
	result->right = std::move(subquery_reference);
	result->condition = make_uniq<ConstantExpression>(Value::BOOLEAN(true));
	return std::move(result);
}

bool PEGTransformerFactory::TransformAsof(PEGTransformer &transformer) {
	return true;
}

JoinType PEGTransformerFactory::TransformFullJoin(PEGTransformer &transformer, const bool &has_result) {
	return JoinType::OUTER;
}

JoinType PEGTransformerFactory::TransformLeftJoin(PEGTransformer &transformer, const bool &has_result) {
	return JoinType::LEFT;
}

JoinType PEGTransformerFactory::TransformRightJoin(PEGTransformer &transformer, const bool &has_result) {
	return JoinType::RIGHT;
}

JoinType PEGTransformerFactory::TransformSemiJoin(PEGTransformer &transformer) {
	return JoinType::SEMI;
}

JoinType PEGTransformerFactory::TransformAntiJoin(PEGTransformer &transformer) {
	return JoinType::ANTI;
}

JoinType PEGTransformerFactory::TransformInnerJoin(PEGTransformer &transformer) {
	return JoinType::INNER;
}

// LeftSemiJoin <- 'LEFT' 'SEMI'
JoinType PEGTransformerFactory::TransformLeftSemiJoin(PEGTransformer &transformer) {
	return JoinType::SEMI;
}

// LeftAntiJoin <- 'LEFT' 'ANTI'
JoinType PEGTransformerFactory::TransformLeftAntiJoin(PEGTransformer &transformer) {
	return JoinType::ANTI;
}

unique_ptr<TableRef> PEGTransformerFactory::TransformTableFunctionLateralOpt(
    PEGTransformer &transformer, const optional<bool> &lateral, const QualifiedName &qualified_table_function,
    vector<FunctionArgument> table_function_arguments, const optional<bool> &with_ordinality,
    const optional<TableAlias> &table_alias) {
	auto result = make_uniq<TableFunctionRef>();

	result->with_ordinality =
	    with_ordinality.value_or(false) ? OrdinalityType::WITH_ORDINALITY : OrdinalityType::WITHOUT_ORDINALITY;
	result->function = make_uniq<FunctionExpression>(qualified_table_function, std::move(table_function_arguments));
	if (table_alias) {
		result->alias = table_alias->name;
		result->column_name_alias = table_alias->column_name_alias;
	}
	auto generator_rows = RewriteStackTableFunction(*result);
	if (generator_rows) {
		return generator_rows;
	}
	return std::move(result);
}

bool PEGTransformerFactory::TransformLateralViewOuter(PEGTransformer &transformer) {
	return true;
}

vector<string> PEGTransformerFactory::TransformLateralViewColumnAliases(PEGTransformer &transformer,
                                                                        const vector<Identifier> &col_label_or_string) {
	return IdentifiersToStrings(col_label_or_string);
}

unique_ptr<TableRef> PEGTransformerFactory::TransformLateralViewClause(
    PEGTransformer &transformer, const optional<bool> &lateral_view_outer,
    const QualifiedName &qualified_table_function, vector<FunctionArgument> table_function_arguments,
    const optional<Identifier> &identifier, const optional<vector<string>> &lateral_view_column_aliases) {
	auto result = make_uniq<TableFunctionRef>();
	auto function_name = qualified_table_function;
	if (lateral_view_outer.value_or(false)) {
		// the OUTER flag selects the generator variant that emits a NULL row for an empty collection
		function_name = function_name.WithName(Identifier(function_name.Name().GetIdentifierName() + "_outer"));
	}
	result->function = make_uniq<FunctionExpression>(function_name, std::move(table_function_arguments));
	if (identifier) {
		result->alias = *identifier;
	}
	if (lateral_view_column_aliases) {
		result->column_name_alias = StringsToIdentifiers(*lateral_view_column_aliases);
	}
	auto generator_rows = RewriteStackTableFunction(*result);
	if (generator_rows) {
		return generator_rows;
	}
	return std::move(result);
}

unique_ptr<TableRef> PEGTransformerFactory::TransformTableFunctionAliasColon(
    PEGTransformer &transformer, const Identifier &table_alias_colon, const QualifiedName &qualified_table_function,
    vector<FunctionArgument> table_function_arguments, const optional<bool> &with_ordinality,
    optional<unique_ptr<SampleOptions>> sample_clause) {
	auto result = make_uniq<TableFunctionRef>();
	result->with_ordinality =
	    with_ordinality.value_or(false) ? OrdinalityType::WITH_ORDINALITY : OrdinalityType::WITHOUT_ORDINALITY;
	result->function = make_uniq<FunctionExpression>(qualified_table_function, std::move(table_function_arguments));
	result->alias = table_alias_colon;
	if (sample_clause) {
		result->sample = std::move(*sample_clause);
	}
	return std::move(result);
}

unique_ptr<SelectStatement>
PEGTransformerFactory::TransformValuesClauseNoParens(PEGTransformer &transformer,
                                                     vector<unique_ptr<ParsedExpression>> expression) {
	auto result = make_uniq<ExpressionListRef>();
	result->alias = "valueslist";
	for (auto &expr : expression) {
		vector<unique_ptr<ParsedExpression>> row;
		row.push_back(std::move(expr));
		result->values.push_back(std::move(row));
	}

	auto select_node = make_uniq<SelectNode>();
	select_node->from_table = std::move(result);
	select_node->select_list.push_back(make_uniq<StarExpression>());

	auto select_statement = make_uniq<SelectStatement>();
	select_statement->node = std::move(select_node);
	return select_statement;
}

unique_ptr<SelectStatement> PEGTransformerFactory::TransformValuesClauseWithAlias(
    PEGTransformer &transformer, unique_ptr<SelectStatement> values_body, const TableAlias &table_alias) {
	auto &select_node = values_body->node->Cast<SelectNode>();
	auto &expr_list_ref = select_node.from_table->Cast<ExpressionListRef>();
	expr_list_ref.alias = table_alias.name;
	expr_list_ref.expected_names = table_alias.column_name_alias;
	return values_body;
}

string PEGTransformerFactory::TransformVersionAtUnit(PEGTransformer &transformer) {
	return "VERSION";
}

string PEGTransformerFactory::TransformTimestampAtUnit(PEGTransformer &transformer) {
	return "TIMESTAMP";
}

OrderType PEGTransformerFactory::TransformDescendingOrder(PEGTransformer &transformer) {
	return OrderType::DESCENDING;
}

OrderType PEGTransformerFactory::TransformAscendingOrder(PEGTransformer &transformer) {
	return OrderType::ASCENDING;
}

OrderByNullType PEGTransformerFactory::TransformNullsFirst(PEGTransformer &transformer) {
	return OrderByNullType::NULLS_FIRST;
}

OrderByNullType PEGTransformerFactory::TransformNullsLast(PEGTransformer &transformer) {
	return OrderByNullType::NULLS_LAST;
}

unique_ptr<ResultModifier> PEGTransformerFactory::VerifyLimitOffset(LimitPercentResult &limit,
                                                                    LimitPercentResult &offset) {
	if (offset.is_percent) {
		throw ParserException("Percentage for offsets are not supported.");
	}
	auto result = make_uniq<LimitModifier>();
	result->limit_type = limit.is_percent ? LimitValueType::PERCENTAGE : LimitValueType::ROW_COUNT;
	if (limit.expression) {
		result->limit = std::move(limit.expression);
	}
	if (offset.expression) {
		result->offset = std::move(offset.expression);
	}
	if (!result->limit && !result->offset) {
		return nullptr;
	}
	return std::move(result);
}

LimitPercentResult PEGTransformerFactory::TransformLimitExpression(PEGTransformer &transformer,
                                                                   unique_ptr<ParsedExpression> expression,
                                                                   const bool &has_result) {
	LimitPercentResult result;
	result.expression = std::move(expression);
	result.is_percent = has_result;
	return result;
}

struct GroupingExpressionMap {
	parsed_expression_map_t<ProjectionIndex> map;
};

static void CheckGroupingSetMax(idx_t count) {
	static constexpr const idx_t MAX_GROUPING_SETS = 65535;
	if (count > MAX_GROUPING_SETS) {
		throw ParserException("Maximum grouping set count of %d exceeded", MAX_GROUPING_SETS);
	}
}

static void CheckGroupingSetCubes(idx_t current_count, idx_t cube_count) {
	idx_t combinations = 1;
	for (idx_t i = 0; i < cube_count; i++) {
		combinations *= 2;
		CheckGroupingSetMax(current_count + combinations);
	}
}

static GroupingSet VectorToGroupingSet(vector<ProjectionIndex> &indexes) {
	GroupingSet result;
	for (idx_t i = 0; i < indexes.size(); i++) {
		result.insert(indexes[i]);
	}
	return result;
}

void PEGTransformerFactory::AddGroupByExpression(unique_ptr<ParsedExpression> expression, GroupingExpressionMap &map,
                                                 GroupByNode &result, vector<ProjectionIndex> &result_set) {
	if (expression->GetExpressionType() == ExpressionType::FUNCTION) {
		auto &func = expression->Cast<FunctionExpression>();
		if (func.FunctionName() == "row") {
			for (auto &child : func.GetArgumentsMutable()) {
				AddGroupByExpression(std::move(child.GetExpressionMutable()), map, result, result_set);
			}
			return;
		}
	}
	auto entry = map.map.find(*expression);
	ProjectionIndex result_idx;
	if (entry == map.map.end()) {
		result_idx = ProjectionIndex(result.group_expressions.size());
		map.map[*expression] = result_idx;
		result.group_expressions.push_back(std::move(expression));
	} else {
		result_idx = entry->second;
	}
	result_set.push_back(result_idx);
}

static void AddCubeSets(const GroupingSet &current_set, vector<GroupingSet> &cube_sets,
                        vector<GroupingSet> &result_sets, idx_t start_idx = 0) {
	CheckGroupingSetMax(result_sets.size());
	result_sets.push_back(current_set);
	for (idx_t k = start_idx; k < cube_sets.size(); k++) {
		auto child_set = current_set;
		child_set.insert(cube_sets[k].begin(), cube_sets[k].end());
		AddCubeSets(child_set, cube_sets, result_sets, k + 1);
	}
}

vector<GroupingSet> PEGTransformerFactory::GroupByExpressionUnfolding(GroupByExpressionInfo &group_by_expr,
                                                                      GroupingExpressionMap &map, GroupByNode &result) {
	vector<GroupingSet> result_sets;
	if (group_by_expr.type == GroupByExpressionInfoType::EMPTY) {
		result_sets.emplace_back();

	} else if (group_by_expr.type == GroupByExpressionInfoType::EXPRESSION) {
		vector<ProjectionIndex> indexes;
		AddGroupByExpression(std::move(group_by_expr.expression), map, result, indexes);
		result_sets.push_back(VectorToGroupingSet(indexes));
	} else if (group_by_expr.type == GroupByExpressionInfoType::GROUPING_SETS) {
		for (auto &child_expr : group_by_expr.children) {
			auto child_sets = GroupByExpressionUnfolding(child_expr, map, result);
			result_sets.insert(result_sets.end(), child_sets.begin(), child_sets.end());
		}
	} else if (group_by_expr.type == GroupByExpressionInfoType::CUBE ||
	           group_by_expr.type == GroupByExpressionInfoType::ROLLUP) {
		if (group_by_expr.expressions.empty()) {
			throw ParserException("CUBE or ROLLUP column list cannot be empty");
		}

		vector<GroupingSet> unfolding_sets;
		for (auto &expr : group_by_expr.expressions) {
			vector<ProjectionIndex> indexes;
			AddGroupByExpression(std::move(expr), map, result, indexes);

			GroupingSet s;
			for (auto idx : indexes) {
				s.insert(idx);
			}
			unfolding_sets.push_back(std::move(s));
		}

		if (group_by_expr.type == GroupByExpressionInfoType::CUBE) {
			CheckGroupingSetCubes(result_sets.size(), unfolding_sets.size());
			GroupingSet current_set;
			AddCubeSets(current_set, unfolding_sets, result_sets, 0);
		} else {
			GroupingSet current_set;
			result_sets.push_back(current_set);
			for (idx_t i = 0; i < unfolding_sets.size(); i++) {
				current_set.insert(unfolding_sets[i].begin(), unfolding_sets[i].end());
				result_sets.push_back(current_set);
			}
		}
	}
	return result_sets;
}

string PEGTransformerFactory::TransformCubeKeyword(PEGTransformer &transformer) {
	return "CUBE";
}

string PEGTransformerFactory::TransformRollupKeyword(PEGTransformer &transformer) {
	return "ROLLUP";
}

GroupByNode PEGTransformerFactory::TransformGroupByList(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	auto group_by_list = ExtractParseResultsFromList(list_pr.Child<ListParseResult>(0));

	GroupByNode result;
	GroupingExpressionMap map;

	// Optional trailing modifier: GROUP BY a, b WITH CUBE/ROLLUP  or  GROUP BY a, b GROUPING SETS (...).
	auto &modifier_opt = list_pr.Child<OptionalParseResult>(1);
	if (modifier_opt.HasResult()) {
		auto &modifier_choice = modifier_opt.GetResult().Cast<ListParseResult>().Child<ChoiceParseResult>(0);
		auto &modifier_result = modifier_choice.GetResult();

		if (StringUtil::CIEquals(modifier_result.name, "WithCubeOrRollup")) {
			// GROUP BY a, b WITH CUBE/ROLLUP is equivalent to GROUP BY CUBE/ROLLUP(a, b).
			auto &with_pr = modifier_result.Cast<ListParseResult>();
			auto type_str = transformer.Transform<string>(with_pr.Child<ListParseResult>(1));
			GroupByExpressionInfo cube_rollup;
			cube_rollup.type = StringUtil::CIEquals(type_str, "CUBE") ? GroupByExpressionInfoType::CUBE
			                                                          : GroupByExpressionInfoType::ROLLUP;
			for (auto &group_by_child : group_by_list) {
				auto info = transformer.Transform<GroupByExpressionInfo>(group_by_child.get());
				if (info.expression) {
					cube_rollup.expressions.push_back(std::move(info.expression));
				}
			}
			result.grouping_sets = GroupByExpressionUnfolding(cube_rollup, map, result);
		} else {
			// GROUP BY a, b, c GROUPING SETS (...): the preceding expressions form the column universe.
			for (auto &group_by_child : group_by_list) {
				auto info = transformer.Transform<GroupByExpressionInfo>(group_by_child.get());
				if (info.expression) {
					vector<ProjectionIndex> indexes;
					AddGroupByExpression(std::move(info.expression), map, result, indexes);
				}
			}
			// TrailingGroupingSets <- 'GROUPING' 'SETS' Parens(GroupByList)
			auto &trailing_gs = modifier_result.Cast<ListParseResult>();
			auto &inner = ExtractResultFromParens(trailing_gs.Child<ListParseResult>(2));
			auto &inner_list_pr = inner.Cast<ListParseResult>();
			auto gs_list = ExtractParseResultsFromList(inner_list_pr.Child<ListParseResult>(0));
			for (auto &child_wrapper : gs_list) {
				auto child_info = transformer.Transform<GroupByExpressionInfo>(child_wrapper.get());
				auto child_sets = GroupByExpressionUnfolding(child_info, map, result);
				result.grouping_sets.insert(result.grouping_sets.end(), child_sets.begin(), child_sets.end());
			}
		}
		return result;
	}

	// No modifier: cartesian-combine each item's grouping sets.
	for (auto &group_by_child : group_by_list) {
		auto info = transformer.Transform<GroupByExpressionInfo>(group_by_child.get());
		vector<GroupingSet> next_sets = GroupByExpressionUnfolding(info, map, result);

		if (result.grouping_sets.empty()) {
			result.grouping_sets = std::move(next_sets);
		} else {
			vector<GroupingSet> new_sets;
			idx_t grouping_set_count = result.grouping_sets.size() * next_sets.size();
			CheckGroupingSetMax(grouping_set_count);
			new_sets.reserve(grouping_set_count);

			for (auto &current_set : result.grouping_sets) {
				for (auto &next_set : next_sets) {
					GroupingSet combined_set;
					combined_set.insert(current_set.begin(), current_set.end());
					combined_set.insert(next_set.begin(), next_set.end());
					new_sets.push_back(std::move(combined_set));
				}
			}
			result.grouping_sets = std::move(new_sets);
		}
	}
	return result;
}

GroupByExpressionInfo PEGTransformerFactory::TransformGroupByBaseExpression(PEGTransformer &transformer,
                                                                            unique_ptr<ParsedExpression> expression) {
	GroupByExpressionInfo result;
	result.type = GroupByExpressionInfoType::EXPRESSION;
	result.expression = std::move(expression);
	return result;
}

GroupByExpressionInfo PEGTransformerFactory::TransformEmptyGroupingItem(PEGTransformer &transformer) {
	GroupByExpressionInfo result;
	result.type = GroupByExpressionInfoType::EMPTY;
	return result;
}

GroupByExpressionInfo
PEGTransformerFactory::TransformCubeOrRollupClause(PEGTransformer &transformer, const string &cube_or_rollup,
                                                   optional<vector<unique_ptr<ParsedExpression>>> expression) {
	GroupByExpressionInfo result;
	result.type = StringUtil::CIEquals(cube_or_rollup, "CUBE") ? GroupByExpressionInfoType::CUBE
	                                                           : GroupByExpressionInfoType::ROLLUP;
	if (expression) {
		result.expressions = std::move(*expression);
	}
	return result;
}

GroupByExpressionInfo
PEGTransformerFactory::TransformGroupingSetsClause(PEGTransformer &transformer,
                                                   vector<GroupByExpressionInfo> group_by_expression) {
	GroupByExpressionInfo result;
	result.type = GroupByExpressionInfoType::GROUPING_SETS;
	result.children = std::move(group_by_expression);
	return result;
}

string PEGTransformerFactory::TransformGroupByModifier(PEGTransformer &transformer, ParseResult &parse_result) {
	throw NotImplementedException("Rule 'GroupByModifier' - handled inline by TransformGroupByList");
}

string PEGTransformerFactory::TransformWithCubeOrRollup(PEGTransformer &transformer, ParseResult &parse_result) {
	throw NotImplementedException("Rule 'WithCubeOrRollup' - handled inline by TransformGroupByList");
}

GroupByNode PEGTransformerFactory::TransformTrailingGroupingSets(PEGTransformer &transformer,
                                                                 ParseResult &parse_result) {
	throw NotImplementedException("Rule 'TrailingGroupingSets' - handled inline by TransformGroupByList");
}

CommonTableExpressionMap PEGTransformerFactory::TransformWithClause(PEGTransformer &transformer,
                                                                    ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	bool is_recursive = list_pr.Child<OptionalParseResult>(1).HasResult();
	auto with_statement_list = PEGTransformerFactory::ExtractParseResultsFromList(list_pr.Child<ListParseResult>(2));
	CommonTableExpressionMap result;

	for (idx_t entry_idx = 0; entry_idx < with_statement_list.size(); entry_idx++) {
		auto with_entry = transformer.Transform<pair<Identifier, unique_ptr<CommonTableExpressionInfo>>>(
		    with_statement_list[entry_idx]);

		if (is_recursive) {
			auto &query_node = with_entry.second->query_node;
			if (!query_node) {
				throw ParserException("Recursive CTEs with DML statements are not supported");
			}
			if (query_node->type == QueryNodeType::INSERT_QUERY_NODE ||
			    query_node->type == QueryNodeType::UPDATE_QUERY_NODE ||
			    query_node->type == QueryNodeType::DELETE_QUERY_NODE) {
				throw ParserException("Recursive CTEs with DML statements are not supported");
			}
			// Now safe to call on SELECT, VALUES, etc.
			query_node = ToRecursiveCTE(std::move(query_node), with_entry.first, with_entry.second->aliases,
			                            with_entry.second->key_targets);
		}
		auto &cte_name = with_entry.first;

		auto it = result.map.find(cte_name);
		if (it != result.map.end()) {
			// can't have two CTEs with same name
			throw ParserException("Duplicate CTE name \"%s\"", cte_name.GetIdentifierName());
		}
		result.map.insert(with_entry.first, std::move(with_entry.second));
	}
	return result;
}

void PEGTransformerFactory::InitializeWithClauseTrampoline(PEGTransformer &transformer, TransformStack &stack,
                                                           TransformStackFrame &frame) {
	auto &list_pr = frame.parse_result.Cast<ListParseResult>();
	auto with_statement_list = ExtractParseResultsFromList(list_pr.Child<ListParseResult>(2));
	frame.ReserveChildSlots(with_statement_list.size());
	for (idx_t i = with_statement_list.size(); i > 0; i--) {
		auto child_idx = i - 1;
		stack.PushFrame(with_statement_list[child_idx].get(), PEGTransformerFactory::GetTrampolineOps("WithStatement"),
		                TransformFrameResultTarget(frame.frame_index, child_idx));
	}
}

unique_ptr<TransformResultValue> PEGTransformerFactory::FinalizeWithClauseTrampoline(PEGTransformer &transformer,
                                                                                     TransformStack &stack,
                                                                                     TransformStackFrame &frame) {
	auto &list_pr = frame.parse_result.Cast<ListParseResult>();
	bool is_recursive = list_pr.Child<OptionalParseResult>(1).HasResult();
	CommonTableExpressionMap result;
	for (idx_t entry_idx = 0; entry_idx < frame.child_results.size(); entry_idx++) {
		auto with_entry = frame.TakeResult<pair<Identifier, unique_ptr<CommonTableExpressionInfo>>>(entry_idx);
		if (is_recursive) {
			auto &query_node = with_entry.second->query_node;
			if (!query_node) {
				throw ParserException("Recursive CTEs with DML statements are not supported");
			}
			if (query_node->type == QueryNodeType::INSERT_QUERY_NODE ||
			    query_node->type == QueryNodeType::UPDATE_QUERY_NODE ||
			    query_node->type == QueryNodeType::DELETE_QUERY_NODE) {
				throw ParserException("Recursive CTEs with DML statements are not supported");
			}
			query_node = ToRecursiveCTE(std::move(query_node), with_entry.first, with_entry.second->aliases,
			                            with_entry.second->key_targets);
		}
		auto &cte_name = with_entry.first;
		auto it = result.map.find(cte_name);
		if (it != result.map.end()) {
			throw ParserException("Duplicate CTE name \"%s\"", cte_name.GetIdentifierName());
		}
		result.map.insert(with_entry.first, std::move(with_entry.second));
	}
	return make_uniq<TypedTransformResult<CommonTableExpressionMap>>(std::move(result));
}

pair<Identifier, unique_ptr<CommonTableExpressionInfo>> PEGTransformerFactory::TransformWithStatement(
    PEGTransformer &transformer, const Identifier &col_id_or_string, const optional<vector<string>> &insert_column_list,
    const bool &has_result, optional<vector<unique_ptr<ParsedExpression>>> using_key,
    const optional<bool> &materialized, unique_ptr<TableRef> cte_body) {
	// has_result is Spark's MAX RECURSION LEVEL clause, a per-CTE recursion ceiling that duckdb does not enforce;
	// ignored.
	auto result = make_uniq<CommonTableExpressionInfo>();
	auto cte_name = col_id_or_string;
	if (insert_column_list) {
		result->aliases = StringsToIdentifiers(*insert_column_list);
	}
	if (using_key) {
		result->key_targets = std::move(*using_key);
	}
	if (materialized) {
		// If this has a result, we know it is either NEVER or ALWAYS
		if (*materialized) {
			result->materialized = CTEMaterialize::CTE_MATERIALIZE_NEVER;
		} else {
			result->materialized = CTEMaterialize::CTE_MATERIALIZE_ALWAYS;
		}
	}
	D_ASSERT(cte_body->type == TableReferenceType::SUBQUERY);
	auto subquery_ref = unique_ptr_cast<TableRef, SubqueryRef>(std::move(cte_body));
	result->query_node = std::move(subquery_ref->subquery->node);
	return make_pair(cte_name, std::move(result));
}

unique_ptr<TableRef>
PEGTransformerFactory::TransformCTESelectBody(PEGTransformer &transformer,
                                              unique_ptr<SelectStatement> select_statement_internal) {
	return make_uniq<SubqueryRef>(std::move(select_statement_internal));
}

unique_ptr<TableRef> PEGTransformerFactory::TransformCTEDMLBody(PEGTransformer &transformer,
                                                                unique_ptr<SQLStatement> statement) {
	unique_ptr<QueryNode> query_node;
	switch (statement->type) {
	case StatementType::INSERT_STATEMENT:
		query_node = unique_ptr_cast<InsertQueryNode, QueryNode>(std::move(statement->Cast<InsertStatement>().node));
		break;
	case StatementType::UPDATE_STATEMENT:
		query_node = unique_ptr_cast<UpdateQueryNode, QueryNode>(std::move(statement->Cast<UpdateStatement>().node));
		break;
	case StatementType::DELETE_STATEMENT:
		query_node = unique_ptr_cast<DeleteQueryNode, QueryNode>(std::move(statement->Cast<DeleteStatement>().node));
		break;
	default:
		throw ParserException("A CTE body must be a SELECT, INSERT, UPDATE, or DELETE statement");
	}
	auto select_statement = make_uniq<SelectStatement>();
	select_statement->node = std::move(query_node);
	return make_uniq<SubqueryRef>(std::move(select_statement));
}

bool PEGTransformerFactory::TransformMaterialized(PEGTransformer &transformer, const bool &has_result) {
	return has_result;
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformWindowDefinition(PEGTransformer &transformer,
                                                                              ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	transformer.in_window_definition = true;
	auto window_function = transformer.Transform<unique_ptr<WindowExpression>>(list_pr.Child<ListParseResult>(2));
	transformer.in_window_definition = false;
	auto window_name = list_pr.Child<IdentifierParseResult>(0).identifier;
	RegisterWindowClause(transformer, window_name, *window_function);
	return std::move(window_function);
}

void PEGTransformerFactory::InitializeWindowDefinitionTrampoline(PEGTransformer &transformer, TransformStack &stack,
                                                                 TransformStackFrame &frame) {
	auto &list_pr = frame.parse_result.Cast<ListParseResult>();
	transformer.in_window_definition = true;
	frame.ReserveChildSlots(1);
	stack.PushFrame(list_pr.GetChild(2), PEGTransformerFactory::GetTrampolineOps("WindowFrameDefinition"),
	                TransformFrameResultTarget(frame.frame_index, 0));
}

unique_ptr<TransformResultValue> PEGTransformerFactory::FinalizeWindowDefinitionTrampoline(PEGTransformer &transformer,
                                                                                           TransformStack &stack,
                                                                                           TransformStackFrame &frame) {
	auto &list_pr = frame.parse_result.Cast<ListParseResult>();
	auto window_function = frame.TakeResult<unique_ptr<WindowExpression>>(0);
	transformer.in_window_definition = false;
	auto window_name = list_pr.Child<IdentifierParseResult>(0).identifier;
	RegisterWindowClause(transformer, window_name, *window_function);
	unique_ptr<ParsedExpression> result = std::move(window_function);
	return make_uniq<TypedTransformResult<unique_ptr<ParsedExpression>>>(std::move(result));
}

unique_ptr<SampleOptions> PEGTransformerFactory::TransformSampleEntryFunction(
    PEGTransformer &transformer, const optional<SampleMethod> &sample_function, unique_ptr<SampleOptions> sample_count,
    const optional<optional_idx> &repeatable_sample) {
	if (sample_function) {
		sample_count->method = *sample_function;
	}
	if (repeatable_sample) {
		sample_count->seed = *repeatable_sample;
		sample_count->repeatable = true;
	}
	return sample_count;
}

unique_ptr<SampleOptions>
PEGTransformerFactory::TransformSampleEntryCount(PEGTransformer &transformer, unique_ptr<SampleOptions> sample_count,
                                                 const optional<pair<SampleMethod, optional_idx>> &sample_properties) {
	if (sample_properties) {
		sample_count->method = sample_properties->first;
		sample_count->seed = sample_properties->second;
		if (sample_count->seed.IsValid()) {
			sample_count->repeatable = true;
		}
	}
	return sample_count;
}

bool PEGTransformerFactory::TransformSamplePercentage(PEGTransformer &transformer) {
	return true;
}

bool PEGTransformerFactory::TransformSampleRows(PEGTransformer &transformer) {
	return false;
}

unique_ptr<SelectStatement>
PEGTransformerFactory::TransformSelectParens(PEGTransformer &transformer,
                                             unique_ptr<SelectStatement> select_statement_internal) {
	return select_statement_internal;
}

vector<unique_ptr<ResultModifier>>
PEGTransformerFactory::TransformResultModifiers(PEGTransformer &transformer,
                                                optional<vector<OrderByNode>> order_by_clause,
                                                optional<unique_ptr<ResultModifier>> limit_offset) {
	vector<unique_ptr<ResultModifier>> result;
	if (order_by_clause) {
		auto order_modifier = make_uniq<OrderModifier>();
		order_modifier->orders = std::move(*order_by_clause);
		result.push_back(std::move(order_modifier));
	}
	if (limit_offset) {
		result.push_back(std::move(*limit_offset));
	}
	return result;
}

unique_ptr<ResultModifier>
PEGTransformerFactory::TransformLimitOffsetClause(PEGTransformer &transformer, LimitPercentResult limit_clause,
                                                  optional<LimitPercentResult> offset_clause) {
	LimitPercentResult empty_offset;
	return VerifyLimitOffset(limit_clause, offset_clause ? *offset_clause : empty_offset);
}

unique_ptr<ResultModifier>
PEGTransformerFactory::TransformOffsetLimitClause(PEGTransformer &transformer, LimitPercentResult offset_clause,
                                                  optional<LimitPercentResult> limit_clause) {
	LimitPercentResult empty_limit;
	return VerifyLimitOffset(limit_clause ? *limit_clause : empty_limit, offset_clause);
}

unique_ptr<ResultModifier> PEGTransformerFactory::TransformOffsetFetchClause(PEGTransformer &transformer,
                                                                             LimitPercentResult offset_clause,
                                                                             LimitPercentResult fetch_clause) {
	return VerifyLimitOffset(fetch_clause, offset_clause);
}

unique_ptr<ResultModifier> PEGTransformerFactory::TransformFetchOnlyClause(PEGTransformer &transformer,
                                                                           LimitPercentResult fetch_clause) {
	LimitPercentResult empty_offset;
	return VerifyLimitOffset(fetch_clause, empty_offset);
}

unique_ptr<SelectStatement> PEGTransformerFactory::TransformTableStatement(PEGTransformer &transformer,
                                                                           unique_ptr<BaseTableRef> base_table_name) {
	auto result = make_uniq<SelectStatement>();
	auto node = make_uniq<SelectNode>();
	node->select_list.push_back(make_uniq<StarExpression>());
	node->from_table = std::move(base_table_name);
	result->node = std::move(node);
	return result;
}

unique_ptr<SelectStatement>
PEGTransformerFactory::TransformSimpleSelectParens(PEGTransformer &transformer,
                                                   unique_ptr<SelectStatement> simple_select) {
	return simple_select;
}

unique_ptr<SelectNode> PEGTransformerFactory::TransformSelectFromClause(PEGTransformer &transformer,
                                                                        unique_ptr<SelectNode> select_clause,
                                                                        optional<unique_ptr<TableRef>> from_clause) {
	if (from_clause) {
		select_clause->from_table = std::move(*from_clause);
	} else {
		select_clause->from_table = make_uniq<EmptyTableRef>();
	}
	return select_clause;
}

unique_ptr<SelectNode>
PEGTransformerFactory::TransformFromSelectClause(PEGTransformer &transformer, unique_ptr<TableRef> from_clause,
                                                 optional<unique_ptr<SelectNode>> select_clause) {
	unique_ptr<SelectNode> result;
	if (!select_clause) {
		result = make_uniq<SelectNode>();
		result->select_list.push_back(make_uniq<StarExpression>());
	} else {
		result = std::move(*select_clause);
	}
	result->from_table = std::move(from_clause);
	return result;
}

vector<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformUsingKey(PEGTransformer &transformer,
                                         vector<unique_ptr<ParsedExpression>> target_list) {
	return target_list;
}

unique_ptr<SelectNode>
PEGTransformerFactory::TransformSelectAllClause(PEGTransformer &transformer,
                                                vector<unique_ptr<ParsedExpression>> target_list) {
	return TransformSelectListClause(transformer, optional<DistinctClause>(), std::move(target_list));
}

unique_ptr<SelectNode>
PEGTransformerFactory::TransformSelectListClause(PEGTransformer &transformer, optional<DistinctClause> distinct_clause,
                                                 optional<vector<unique_ptr<ParsedExpression>>> target_list) {
	auto result = make_uniq<SelectNode>();
	if (distinct_clause && distinct_clause->is_distinct) {
		auto distinct_modifier = make_uniq<DistinctModifier>();
		for (auto &distinct_on : distinct_clause->distinct_targets) {
			distinct_modifier->distinct_on_targets.push_back(distinct_on->Copy());
		}
		result->modifiers.push_back(std::move(distinct_modifier));
	}
	if (!target_list) {
		throw ParserException("SELECT clause without selection list");
	}
	for (auto &expr_ptr : *target_list) {
		result->select_list.push_back(std::move(expr_ptr));
	}
	return result;
}

vector<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformTargetList(PEGTransformer &transformer,
                                           vector<unique_ptr<ParsedExpression>> aliased_expression) {
	return aliased_expression;
}

vector<string> PEGTransformerFactory::TransformColumnAliases(PEGTransformer &transformer,
                                                             const vector<Identifier> &col_label_or_string) {
	return IdentifiersToStrings(col_label_or_string);
}

DistinctClause PEGTransformerFactory::TransformDistinctAll(PEGTransformer &transformer, ParseResult &parse_result) {
	DistinctClause result;
	result.is_distinct = false;
	return result;
}

DistinctClause
PEGTransformerFactory::TransformDistinctOn(PEGTransformer &transformer,
                                           optional<vector<unique_ptr<ParsedExpression>>> distinct_on_targets) {
	DistinctClause result;
	result.is_distinct = true;
	if (distinct_on_targets) {
		result.distinct_targets = std::move(*distinct_on_targets);
	}
	return result;
}

vector<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformDistinctOnTargets(PEGTransformer &transformer,
                                                  vector<unique_ptr<ParsedExpression>> expression) {
	return expression;
}

unique_ptr<TableRef> PEGTransformerFactory::TransformTableSubquery(PEGTransformer &transformer,
                                                                   const optional<bool> &lateral,
                                                                   unique_ptr<TableRef> subquery_reference,
                                                                   const optional<TableAlias> &table_alias) {
	if (table_alias) {
		subquery_reference->alias = table_alias->name;
		subquery_reference->column_name_alias = table_alias->column_name_alias;
	}
	RewriteOuterQualifiedStars(*subquery_reference);
	return subquery_reference;
}

unique_ptr<TableRef> PEGTransformerFactory::TransformBaseTableRef(PEGTransformer &transformer,
                                                                  const optional<Identifier> &table_alias_colon,
                                                                  unique_ptr<BaseTableRef> base_table_name,
                                                                  const optional<TableAlias> &table_alias,
                                                                  optional<unique_ptr<AtClause>> at_clause,
                                                                  optional<unique_ptr<SampleOptions>> sample_clause) {
	if (table_alias_colon) {
		base_table_name->alias = *table_alias_colon;
	}
	if (table_alias && table_alias_colon) {
		throw ParserException("Table reference %s cannot have two aliases", base_table_name->ToString());
	}
	if (table_alias) {
		base_table_name->alias = table_alias->name;
		base_table_name->column_name_alias = table_alias->column_name_alias;
	}
	if (at_clause) {
		base_table_name->at_clause = std::move(*at_clause);
	}
	if (sample_clause) {
		base_table_name->sample = std::move(*sample_clause);
	}
	return std::move(base_table_name);
}

Identifier PEGTransformerFactory::TransformTableAliasColon(PEGTransformer &transformer,
                                                           const Identifier &col_id_or_string) {
	return col_id_or_string;
}

unique_ptr<TableRef> PEGTransformerFactory::TransformValuesRef(PEGTransformer &transformer,
                                                               unique_ptr<SelectStatement> values_body,
                                                               const optional<TableAlias> &table_alias) {
	auto subquery_ref = make_uniq<SubqueryRef>(std::move(values_body));
	if (table_alias) {
		subquery_ref->alias = table_alias->name;
		subquery_ref->column_name_alias = table_alias->column_name_alias;
	}
	return std::move(subquery_ref);
}

unique_ptr<TableRef> PEGTransformerFactory::TransformParensTableRef(PEGTransformer &transformer,
                                                                    const optional<Identifier> &table_alias_colon,
                                                                    unique_ptr<TableRef> table_ref,
                                                                    const optional<TableAlias> &table_alias,
                                                                    optional<unique_ptr<SampleOptions>> sample_clause) {
	if (table_alias_colon && table_alias) {
		throw ParserException("Table reference %s cannot have two aliases", table_ref->ToString());
	}
	if (!table_alias_colon && !table_alias && !sample_clause) {
		return table_ref;
	}
	auto select_statement = make_uniq<SelectStatement>();
	auto select_node = make_uniq<SelectNode>();
	select_node->select_list.push_back(make_uniq<StarExpression>());
	select_node->from_table = std::move(table_ref);
	select_statement->node = std::move(select_node);
	auto subquery = make_uniq<SubqueryRef>(std::move(select_statement));
	if (table_alias_colon) {
		subquery->alias = *table_alias_colon;
	}
	if (table_alias) {
		subquery->alias = table_alias->name;
		subquery->column_name_alias = table_alias->column_name_alias;
	}
	if (sample_clause) {
		subquery->sample = std::move(*sample_clause);
	}
	return std::move(subquery);
}

vector<string> PEGTransformerFactory::TransformPivotGroupByList(PEGTransformer &transformer,
                                                                const vector<Identifier> &col_id_or_string) {
	return IdentifiersToStrings(col_id_or_string);
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformPivotHeader(PEGTransformer &transformer,
                                                                         unique_ptr<ParsedExpression> base_expression) {
	return base_expression;
}

PivotColumn PEGTransformerFactory::TransformUnpivotValueList(PEGTransformer &transformer,
                                                             const vector<string> &unpivot_header,
                                                             vector<PivotColumnEntry> unpivot_target_list) {
	PivotColumn result;
	result.unpivot_names = StringsToIdentifiers(unpivot_header);
	if (result.unpivot_names.size() != 1) {
		throw ParserException("UNPIVOT requires a single column name for the PIVOT IN clause");
	}
	result.entries = std::move(unpivot_target_list);
	return result;
}

vector<PivotColumnEntry>
PEGTransformerFactory::TransformPivotTargetList(PEGTransformer &transformer,
                                                vector<unique_ptr<ParsedExpression>> target_list) {
	vector<PivotColumnEntry> result;
	for (auto &target : target_list) {
		PivotColumnEntry pivot_entry;
		pivot_entry.alias = target->GetAlias();
		bool transformed = TransformPivotInList(target, pivot_entry);
		if (!transformed) {
			pivot_entry.expr = std::move(target);
		}
		result.push_back(std::move(pivot_entry));
	}
	return result;
}

vector<PivotColumnEntry>
PEGTransformerFactory::TransformUnpivotTargetList(PEGTransformer &transformer,
                                                  vector<unique_ptr<ParsedExpression>> target_list) {
	vector<PivotColumnEntry> result;
	for (auto &target : target_list) {
		PivotColumnEntry pivot_entry;
		pivot_entry.alias = target->GetAlias();
		pivot_entry.expr = std::move(target);
		result.push_back(std::move(pivot_entry));
	}
	return result;
}

unique_ptr<BaseTableRef> PEGTransformerFactory::TransformSchemaReservedTable(PEGTransformer &transformer,
                                                                             const Identifier &schema_qualification,
                                                                             const Identifier &reserved_table_name) {
	const auto description = TableDescription(QualifiedName({schema_qualification}, reserved_table_name));
	return make_uniq<BaseTableRef>(description);
}

unique_ptr<BaseTableRef> PEGTransformerFactory::TransformCatalogReservedSchemaTable(
    PEGTransformer &transformer, const Identifier &catalog_qualification,
    const Identifier &reserved_schema_qualification, const Identifier &reserved_table_name) {
	const auto description =
	    TableDescription(QualifiedName(catalog_qualification, reserved_schema_qualification, reserved_table_name));
	return make_uniq<BaseTableRef>(description);
}

QualifiedName PEGTransformerFactory::TransformQualifiedTableFunction(PEGTransformer &transformer,
                                                                     const optional<Identifier> &catalog_qualification,
                                                                     const optional<Identifier> &schema_qualification,
                                                                     const Identifier &table_function_name) {
	Identifier catalog = catalog_qualification ? *catalog_qualification : Identifier();
	Identifier schema = schema_qualification ? *schema_qualification : Identifier();
	if (!catalog.empty() && schema.empty()) {
		schema = std::move(catalog);
		catalog = Identifier();
	}
	return QualifiedName(std::move(catalog), std::move(schema), table_function_name);
}

vector<FunctionArgument>
PEGTransformerFactory::TransformTableFunctionArguments(PEGTransformer &transformer,
                                                       optional<vector<FunctionArgument>> function_argument) {
	if (function_argument) {
		return std::move(*function_argument);
	}
	return {};
}

TableAlias PEGTransformerFactory::TransformTableAliasAs(PEGTransformer &transformer,
                                                        const QualifiedName &identifier_or_string_literal,
                                                        const optional<vector<string>> &column_aliases) {
	TableAlias result;
	result.name = identifier_or_string_literal.Name();
	if (column_aliases) {
		result.column_name_alias = StringsToIdentifiers(*column_aliases);
	}
	return result;
}

TableAlias PEGTransformerFactory::TransformTableAliasWithoutAs(PEGTransformer &transformer,
                                                               const Identifier &identifier,
                                                               const optional<vector<string>> &column_aliases) {
	TableAlias result;
	result.name = identifier;
	if (column_aliases) {
		result.column_name_alias = StringsToIdentifiers(*column_aliases);
	}
	return result;
}

unique_ptr<AtClause> PEGTransformerFactory::TransformAtClause(PEGTransformer &transformer,
                                                              unique_ptr<AtClause> at_specifier) {
	return at_specifier;
}

unique_ptr<AtClause> PEGTransformerFactory::TransformAtSpecifier(PEGTransformer &transformer, const string &at_unit,
                                                                 unique_ptr<ParsedExpression> expression) {
	return make_uniq<AtClause>(at_unit, std::move(expression));
}

unique_ptr<TableRef> PEGTransformerFactory::TransformJoinWithoutOnClause(PEGTransformer &transformer,
                                                                         const JoinPrefix &join_prefix,
                                                                         unique_ptr<TableRef> table_ref,
                                                                         optional<JoinQualifier> join_qualifier) {
	auto result = make_uniq<JoinRef>();
	result->ref_type = join_prefix.ref_type;
	result->type = join_prefix.join_type;
	result->right = std::move(table_ref);
	if (join_qualifier) {
		if (join_qualifier->on_clause) {
			result->condition = std::move(join_qualifier->on_clause);
		} else if (!join_qualifier->using_columns.empty()) {
			result->using_columns = std::move(join_qualifier->using_columns);
		}
		// CROSS JOIN with a condition is semantically an INNER JOIN
		if (result->ref_type == JoinRefType::CROSS) {
			result->ref_type = JoinRefType::REGULAR;
			result->type = JoinType::INNER;
		}
	}
	return std::move(result);
}

JoinQualifier PEGTransformerFactory::TransformOnClause(PEGTransformer &transformer,
                                                       unique_ptr<ParsedExpression> expression) {
	JoinQualifier result;
	result.on_clause = std::move(expression);
	return result;
}

JoinQualifier PEGTransformerFactory::TransformUsingClause(PEGTransformer &transformer,
                                                          const vector<Identifier> &column_name) {
	JoinQualifier result;
	for (auto &col_identifier : column_name) {
		if (col_identifier.empty()) {
			throw ParserException("Column identifier cannot be empty");
		}
		result.using_columns.push_back(col_identifier);
	}
	return result;
}

JoinPrefix PEGTransformerFactory::TransformCrossJoinPrefix(PEGTransformer &transformer) {
	JoinPrefix result;
	result.ref_type = JoinRefType::CROSS;
	return result;
}

JoinPrefix PEGTransformerFactory::TransformNaturalJoinPrefix(PEGTransformer &transformer,
                                                             const optional<JoinType> &join_type) {
	JoinPrefix result;
	result.ref_type = JoinRefType::NATURAL;
	result.join_type = join_type.value_or(JoinType::INNER);
	return result;
}

JoinPrefix PEGTransformerFactory::TransformPositionalJoinPrefix(PEGTransformer &transformer) {
	JoinPrefix result;
	result.ref_type = JoinRefType::POSITIONAL;
	return result;
}

//! `(SELECT *)` without a FROM clause is Spark's one-row, zero-column relation: `*` expands over the
//! input columns of the query, and a SELECT without a FROM clause has none.
static bool IsFromlessStarSubquery(TableRef &ref) {
	auto select_node = FromlessSubquerySelect(ref);
	if (!select_node) {
		return false;
	}
	// anything that can change the cardinality means the relation is not a plain single row
	if (select_node->where_clause || select_node->having || select_node->qualify || select_node->sample) {
		return false;
	}
	if (!select_node->groups.group_expressions.empty() || !select_node->modifiers.empty()) {
		return false;
	}
	if (select_node->select_list.size() != 1) {
		return false;
	}
	return IsPlainUnqualifiedStar(*select_node->select_list[0]);
}

static unique_ptr<TableRef> ImplicitCrossProduct(unique_ptr<TableRef> left, unique_ptr<TableRef> right) {
	auto cross_product = make_uniq<JoinRef>();
	cross_product->left = std::move(left);
	cross_product->right = std::move(right);
	cross_product->ref_type = JoinRefType::CROSS;
	cross_product->is_implicit = true;
	return std::move(cross_product);
}

unique_ptr<TableRef>
PEGTransformerFactory::TransformFromClause(PEGTransformer &transformer, vector<unique_ptr<TableRef>> table_ref,
                                           optional<vector<unique_ptr<TableRef>>> lateral_view_clause) {
	unique_ptr<TableRef> result_table_ref;
	for (auto &entry : table_ref) {
		// cross joining a one-row, zero-column relation is an identity, so it is dropped from the FROM list
		if (IsFromlessStarSubquery(*entry)) {
			continue;
		}
		if (!result_table_ref) {
			result_table_ref = std::move(entry);
			continue;
		}
		result_table_ref = ImplicitCrossProduct(std::move(result_table_ref), std::move(entry));
	}
	if (!result_table_ref) {
		// the query would produce zero columns, which has no representation - keep the entry so binding reports it
		result_table_ref = std::move(table_ref[0]);
	}
	if (lateral_view_clause) {
		// each generator applies to the relation to its left, so the clauses associate left to right
		for (auto &lateral_view : *lateral_view_clause) {
			result_table_ref = ImplicitCrossProduct(std::move(result_table_ref), std::move(lateral_view));
		}
	}
	return result_table_ref;
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformWhereClause(PEGTransformer &transformer,
                                                                         unique_ptr<ParsedExpression> expression) {
	return expression;
}

GroupByNode PEGTransformerFactory::TransformGroupByClause(PEGTransformer &transformer,
                                                          GroupByNode group_by_expressions) {
	return group_by_expressions;
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformHavingClause(PEGTransformer &transformer,
                                                                          unique_ptr<ParsedExpression> expression) {
	return expression;
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformQualifyClause(PEGTransformer &transformer,
                                                                           unique_ptr<ParsedExpression> expression) {
	return expression;
}

unique_ptr<SampleOptions> PEGTransformerFactory::TransformSampleClause(PEGTransformer &transformer,
                                                                       unique_ptr<SampleOptions> sample_entry) {
	return sample_entry;
}

vector<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformWindowClause(PEGTransformer &transformer,
                                             vector<unique_ptr<ParsedExpression>> window_definition) {
	return window_definition;
}

SampleMethod PEGTransformerFactory::TransformSampleFunction(PEGTransformer &transformer, const Identifier &col_id) {
	return EnumUtil::FromString<SampleMethod>(col_id.GetIdentifierName());
}

pair<SampleMethod, optional_idx>
PEGTransformerFactory::TransformSampleProperties(PEGTransformer &transformer, const Identifier &col_id,
                                                 const optional<optional_idx> &sample_seed) {
	return make_pair(EnumUtil::FromString<SampleMethod>(col_id.GetIdentifierName()),
	                 sample_seed ? *sample_seed : optional_idx());
}

optional_idx PEGTransformerFactory::TransformRepeatableSample(PEGTransformer &transformer,
                                                              const optional_idx &sample_seed) {
	return sample_seed;
}

optional_idx PEGTransformerFactory::TransformSampleSeed(PEGTransformer &transformer,
                                                        unique_ptr<ParsedExpression> number_literal) {
	auto const_expr = number_literal->Cast<ConstantExpression>();
	return optional_idx(const_expr.GetValue().GetValue<idx_t>());
}

unique_ptr<SampleOptions> PEGTransformerFactory::TransformSampleCount(PEGTransformer &transformer,
                                                                      unique_ptr<ParsedExpression> sample_value,
                                                                      const optional<bool> &sample_unit) {
	auto result = make_uniq<SampleOptions>();
	if (sample_value->GetExpressionClass() != ExpressionClass::CONSTANT) {
		throw ParserException(sample_value->GetQueryLocation(),
		                      "Only constants are supported in sample clause currently");
	}
	auto &const_expr = sample_value->Cast<ConstantExpression>();
	auto &sample_value_const = const_expr.GetValue();
	result->is_percentage = sample_unit.value_or(false);
	if (result->is_percentage) {
		auto percentage = sample_value_const.GetValue<double>();
		if (percentage < 0 || percentage > 100) {
			throw ParserException("Sample sample_size %llf out of range, must be between 0 and 100", percentage);
		}
		result->sample_size = Value::DOUBLE(percentage);
		result->method = SampleMethod::SYSTEM_SAMPLE;
	} else {
		auto rows = sample_value_const.GetValue<int64_t>();
		if (rows < 0 || sample_value_const.GetValue<uint64_t>() > SampleOptions::MAX_SAMPLE_ROWS) {
			throw ParserException("Sample rows %lld out of range, must be between 0 and %lld", rows,
			                      SampleOptions::MAX_SAMPLE_ROWS);
		}
		result->sample_size = Value::BIGINT(rows);
		result->method = SampleMethod::RESERVOIR_SAMPLE;
	}
	return result;
}

GroupByNode PEGTransformerFactory::TransformGroupByAll(PEGTransformer &transformer) {
	GroupByNode result;
	result.group_expressions.push_back(make_uniq<StarExpression>());
	return result;
}

unique_ptr<TableRef>
PEGTransformerFactory::TransformSubqueryReference(PEGTransformer &transformer,
                                                  unique_ptr<SelectStatement> select_statement_internal) {
	return make_uniq<SubqueryRef>(std::move(select_statement_internal));
}

OrderByNode PEGTransformerFactory::TransformOrderByExpression(PEGTransformer &transformer,
                                                              unique_ptr<ParsedExpression> expression,
                                                              const optional<OrderType> &desc_or_asc,
                                                              const optional<OrderByNullType> &nulls_first_or_last) {
	return OrderByNode(desc_or_asc.value_or(OrderType::ORDER_DEFAULT),
	                   nulls_first_or_last.value_or(OrderByNullType::ORDER_DEFAULT), std::move(expression));
}

vector<OrderByNode> PEGTransformerFactory::TransformOrderByClause(PEGTransformer &transformer,
                                                                  vector<OrderByNode> order_by_expressions) {
	return order_by_expressions;
}

vector<OrderByNode> PEGTransformerFactory::TransformOrderByExpressionList(PEGTransformer &transformer,
                                                                          vector<OrderByNode> order_by_expression) {
	return order_by_expression;
}

vector<OrderByNode> PEGTransformerFactory::TransformOrderByAll(PEGTransformer &transformer,
                                                               const optional<OrderType> &desc_or_asc,
                                                               const optional<OrderByNullType> &nulls_first_or_last) {
	vector<OrderByNode> result;
	auto star_expr = make_uniq<StarExpression>();
	star_expr->IsColumnsMutable() = true;
	result.push_back(OrderByNode(desc_or_asc.value_or(OrderType::ORDER_DEFAULT),
	                             nulls_first_or_last.value_or(OrderByNullType::ORDER_DEFAULT), std::move(star_expr)));
	return result;
}

LimitPercentResult PEGTransformerFactory::TransformLimitClause(PEGTransformer &transformer,
                                                               LimitPercentResult limit_value) {
	return limit_value;
}

LimitPercentResult PEGTransformerFactory::TransformOffsetClause(PEGTransformer &transformer,
                                                                LimitPercentResult offset_value) {
	return offset_value;
}

LimitPercentResult PEGTransformerFactory::TransformOffsetValue(PEGTransformer &transformer,
                                                               unique_ptr<ParsedExpression> expression,
                                                               const bool &has_result) {
	LimitPercentResult result;
	result.expression = std::move(expression);
	return result;
}

LimitPercentResult PEGTransformerFactory::TransformLimitAll(PEGTransformer &transformer) {
	LimitPercentResult result;
	result.expression = make_uniq<ConstantExpression>(Value());
	result.is_percent = false;
	return result;
}

LimitPercentResult PEGTransformerFactory::TransformLimitLiteralPercent(PEGTransformer &transformer,
                                                                       unique_ptr<ParsedExpression> number_literal) {
	LimitPercentResult result;
	result.expression = std::move(number_literal);
	result.is_percent = true;
	return result;
}

LimitPercentResult PEGTransformerFactory::TransformFetchValue(PEGTransformer &transformer,
                                                              unique_ptr<ParsedExpression> expression) {
	LimitPercentResult result;
	result.expression = std::move(expression);
	return result;
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformColIdExpression(PEGTransformer &transformer,
                                                                             const Identifier &col_id,
                                                                             unique_ptr<ParsedExpression> expression) {
	expression->SetAlias(col_id);
	return expression;
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformExpressionAsCollabel(
    PEGTransformer &transformer, unique_ptr<ParsedExpression> expression, const Identifier &col_label_or_string) {
	expression->SetAlias(col_label_or_string);
	return expression;
}

//! Spark's generators (posexplode etc.) name their output columns with a parenthesized multi-column
//! alias `AS (c1, c2, ...)` in the SELECT list. Rewrite a supported generator call into
//! `unnest(list_transform(<entries>(arg), row -> struct_pack(c1 := struct_extract_at(row, 1), ...)), max_depth := 2)`:
//! the entries helper returns a LIST(STRUCT(...)) row-per-element, the struct is renamed field-by-field to
//! the aliases (positionally, so array/map shape does not matter), and the struct-expanding unnest at the
//! select-item root turns each struct field into an output column. A parenthesized alias list is only valid
//! on a generator, so anything else is a parse error.
unique_ptr<ParsedExpression> PEGTransformerFactory::TransformExpressionAsColumnAliases(
    PEGTransformer &transformer, unique_ptr<ParsedExpression> expression, const vector<string> &column_aliases) {
	const char *entries_fn = nullptr;
	if (expression->GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &func = expression->Cast<FunctionExpression>();
		entries_fn = GeneratorEntriesFunction(StringUtil::Lower(func.FunctionName().GetIdentifierName()));
		if (!IsBareGeneratorCall(func)) {
			entries_fn = nullptr;
		}
	}
	if (!entries_fn) {
		throw ParserException(
		    "A parenthesized column alias list is only supported on a generator function (e.g. posexplode)");
	}
	auto &func = expression->Cast<FunctionExpression>();

	vector<unique_ptr<ParsedExpression>> entries_args;
	entries_args.push_back(std::move(func.GetArgumentsMutable()[0].GetExpressionMutable()));
	auto entries = make_uniq<FunctionExpression>(Identifier(entries_fn), std::move(entries_args));

	// row -> struct_pack(alias_0 := struct_extract_at(row, 1), ..., alias_{n-1} := struct_extract_at(row, n))
	// Spark allows the same alias twice, but struct fields cannot repeat a name: a repeated alias takes
	// the `_1` suffix duckdb gives any other duplicate column label.
	auto field_names = column_aliases;
	QueryResult::DeduplicateColumns(field_names);
	const string lambda_param = "__spark_gen_row";
	vector<FunctionArgument> struct_fields;
	for (idx_t i = 0; i < field_names.size(); i++) {
		vector<unique_ptr<ParsedExpression>> extract_args;
		extract_args.push_back(make_uniq<ColumnRefExpression>(Identifier(lambda_param)));
		extract_args.push_back(make_uniq<ConstantExpression>(Value::INTEGER(UnsafeNumericCast<int32_t>(i + 1))));
		struct_fields.emplace_back(
		    Identifier(field_names[i]),
		    make_uniq<FunctionExpression>(Identifier("struct_extract_at"), std::move(extract_args)));
	}
	auto struct_pack = make_uniq<FunctionExpression>(Identifier("struct_pack"), std::move(struct_fields));
	auto lambda = make_uniq<LambdaExpression>(vector<string> {lambda_param}, std::move(struct_pack));

	vector<unique_ptr<ParsedExpression>> transform_args;
	transform_args.push_back(std::move(entries));
	transform_args.push_back(std::move(lambda));
	auto list_transform = make_uniq<FunctionExpression>(Identifier("list_transform"), std::move(transform_args));

	auto max_depth = make_uniq<ConstantExpression>(Value::INTEGER(2));
	max_depth->SetAlias(Identifier("max_depth"));
	vector<unique_ptr<ParsedExpression>> unnest_args;
	unnest_args.push_back(std::move(list_transform));
	unnest_args.push_back(std::move(max_depth));
	return make_uniq<FunctionExpression>(Identifier("unnest"), std::move(unnest_args));
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformExpressionOptIdentifier(
    PEGTransformer &transformer, unique_ptr<ParsedExpression> expression, const optional<Identifier> &identifier) {
	if (identifier) {
		expression->SetAlias(*identifier);
	}
	return expression;
}

unique_ptr<SelectStatement>
PEGTransformerFactory::TransformValuesClause(PEGTransformer &transformer,
                                             vector<vector<unique_ptr<ParsedExpression>>> values_expressions) {
	if (values_expressions.size() > 1) {
		const auto expected_size = values_expressions[0].size();
		for (idx_t i = 1; i < values_expressions.size(); i++) {
			if (values_expressions[i].size() != expected_size) {
				throw ParserException(
				    *values_expressions[i][0],
				    "VALUES lists must all be the same length, expected %d %s but found a list with %d %s",
				    expected_size, expected_size == 1 ? "entry" : "entries", values_expressions[i].size(),
				    values_expressions[i].size() == 1 ? "entry" : "entries");
			}
		}
	}
	auto result = make_uniq<ExpressionListRef>();
	result->alias = "valueslist";
	result->values = std::move(values_expressions);
	if (result->expected_names.empty()) {
		// push col1, col2, col3 (for Spark)
		for (idx_t i = 0; i < result->values[0].size(); i++) {
			result->expected_names.emplace_back("col" + to_string(i + 1));
		}
	}
	auto select_node = make_uniq<SelectNode>();
	select_node->from_table = std::move(result);
	select_node->select_list.push_back(make_uniq<StarExpression>());
	auto select_statement = make_uniq<SelectStatement>();
	select_statement->node = std::move(select_node);
	return select_statement;
}

vector<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformValuesExpressions(PEGTransformer &transformer,
                                                  vector<unique_ptr<ParsedExpression>> row_expression_arg) {
	vector<unique_ptr<ParsedExpression>> result;
	for (auto &expr : row_expression_arg) {
		// Spark accepts an inline AS alias inside a VALUES row but ignores it; column names come
		// from the table alias (or default col1, col2, ...), so discard the per-element alias here.
		expr->ClearAlias();
		result.push_back(std::move(expr));
	}
	return result;
}

} // namespace duckdb
