#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/statement/set_statement.hpp"

namespace duckdb_fork {
using namespace duckdb;

// DeclareStatement <- 'DECLARE' OrReplace? 'VARIABLE'? Identifier Type? DeclareValue?
// Spark session variable -> duckdb SET VARIABLE. The declared type and OR REPLACE are accepted but not
// enforced: duckdb variables are dynamically typed and SET VARIABLE already replaces an existing value.
unique_ptr<SQLStatement> PEGTransformerFactory::TransformDeclareStatement(
    PEGTransformer &transformer, const optional<bool> &or_replace, const bool &has_result, const Identifier &identifier,
    const optional<LogicalType> &type, optional<unique_ptr<ParsedExpression>> declare_value) {
	unique_ptr<ParsedExpression> value;
	if (declare_value) {
		value = std::move(*declare_value);
	} else {
		// An unassigned Spark variable initializes to NULL.
		value = make_uniq<ConstantExpression>(Value());
	}
	return make_uniq<SetVariableStatement>(identifier, std::move(value), SetScope::VARIABLE);
}

// DropVariableStatement <- 'DROP' ('TEMPORARY' / 'TEMP')? 'VARIABLE' IfExists? Identifier
unique_ptr<SQLStatement> PEGTransformerFactory::TransformDropVariableStatement(PEGTransformer &transformer,
                                                                               const bool &has_result,
                                                                               const optional<bool> &if_exists,
                                                                               const Identifier &identifier) {
	return make_uniq<ResetVariableStatement>(identifier, SetScope::VARIABLE);
}

} // namespace duckdb_fork
