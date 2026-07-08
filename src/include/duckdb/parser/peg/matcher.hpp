//===----------------------------------------------------------------------===//
//                         DuckDB
//
// matcher.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parser_extension.hpp"
#include "duckdb/parser/peg/transformer/parse_result.hpp"
#include <mutex>
#include <cctype>

namespace duckdb_fork {
using namespace duckdb;

//! Spark-compat parser helper (formerly its own header duckdb/spark_compat.h).
//! Recognises Spark numeric-literal suffixes (1L, 1S, 1BD, ...) during parsing.
class SparkCompatUtils {
public:
	bool static IsSparkPostfixToken(const string &str) {
		auto c = std::toupper(str[0]);
		if (str.size() == 1) {
			return (c == 'L' || c == 'S' || c == 'Y' || c == 'D' || c == 'F');
		}
		if (str.size() == 2 && c == 'B' && std::toupper(str[1]) == 'D') {
			return true;
		}
		return false;
	}
};
class PEGTransformerFactory;
class ParseResultAllocator;
class Matcher;
class MatcherAllocator;
// Declared here so PEGMatcher's `friend struct ParserCache` binds to this fork type,
// not duckdb::ParserCache (visible via `using namespace duckdb`); MSVC mis-binds otherwise.
struct ParserCache;

enum class SuggestionState : uint8_t {
	SUGGEST_KEYWORD,
	SUGGEST_CATALOG_NAME,
	SUGGEST_SCHEMA_NAME,
	SUGGEST_TABLE_NAME,
	SUGGEST_TYPE_NAME,
	SUGGEST_COLUMN_NAME,
	SUGGEST_FILE_NAME,
	SUGGEST_DIRECTORY,
	SUGGEST_VARIABLE,
	SUGGEST_SCALAR_FUNCTION_NAME,
	SUGGEST_TABLE_FUNCTION_NAME,
	SUGGEST_PRAGMA_NAME,
	SUGGEST_SETTING_NAME,
	SUGGEST_RESERVED_VARIABLE
};

enum class CandidateType { KEYWORD, IDENTIFIER, LITERAL };

struct AutoCompleteCandidate {
	// NOLINTNEXTLINE: allow implicit conversion from string
	AutoCompleteCandidate(string candidate_p, SuggestionState suggestion_type, int32_t score_bonus = 0,
	                      CandidateType candidate_type = CandidateType::IDENTIFIER)
	    : candidate(std::move(candidate_p)), suggestion_type(suggestion_type), score_bonus(score_bonus),
	      candidate_type(candidate_type) {
	}
	// NOLINTNEXTLINE: allow implicit conversion from const char*
	AutoCompleteCandidate(const char *candidate_p, SuggestionState suggestion_type, int32_t score_bonus = 0,
	                      CandidateType candidate_type = CandidateType::IDENTIFIER)
	    : AutoCompleteCandidate(string(candidate_p), suggestion_type, score_bonus, candidate_type) {
	}
	// NOLINTNEXTLINE: allow implicit conversion from Identifier
	AutoCompleteCandidate(const Identifier &candidate_p, SuggestionState suggestion_type, int32_t score_bonus = 0,
	                      CandidateType candidate_type = CandidateType::IDENTIFIER)
	    : AutoCompleteCandidate(candidate_p.GetIdentifierName(), suggestion_type, score_bonus, candidate_type) {
	}

	string candidate;
	//! Type being suggested
	SuggestionState suggestion_type;
	//! The higher the score bonus, the more likely this candidate will be chosen
	int32_t score_bonus;
	//! The type of candidate we are suggesting - this modifies how we handle quoting/case sensitivity
	CandidateType candidate_type;
	//! Extra char to push at the back
	char extra_char = '\0';
	//! Suggestion position
	idx_t suggestion_pos = 0;
	//! The final score
	optional_idx score;
};

struct AutoCompleteSuggestion {
	AutoCompleteSuggestion(string text_p, idx_t pos, string type_p, idx_t score, char extra_char_p)
	    : text(std::move(text_p)), pos(pos), type(std::move(type_p)), score(score), extra_char(extra_char_p) {
	}

	string text;
	idx_t pos;
	string type;
	idx_t score;
	char extra_char;
};

enum class MatchResultType { SUCCESS, FAIL };

enum class SuggestionType { OPTIONAL, MANDATORY };

struct MatcherToken {
	// NOLINTNEXTLINE: allow implicit conversion from text
	MatcherToken(string text_p, idx_t offset_p, TokenType type_p, bool unterminated_p = false)
	    : type(type_p), text(std::move(text_p)), offset(offset_p), unterminated(unterminated_p) {
		length = text.length();
	}

	TokenType type;
	string text;
	idx_t offset = 0;
	idx_t length = 0;
	bool unterminated = false;
};

struct MatcherSuggestion {
	// NOLINTNEXTLINE: allow implicit conversion from auto-complete candidate
	MatcherSuggestion(AutoCompleteCandidate keyword_p) : keyword(std::move(keyword_p)), type(keyword.suggestion_type) {
	}
	// NOLINTNEXTLINE: allow implicit conversion from suggestion state
	MatcherSuggestion(SuggestionState type, char extra_char = '\0')
	    : keyword("", type), type(type), extra_char(extra_char) {
	}

	//! Literal suggestion
	AutoCompleteCandidate keyword;
	SuggestionState type;
	char extra_char = '\0';
};

struct MatchState {
	MatchState(vector<MatcherToken> &tokens, vector<MatcherSuggestion> &suggestions, ParseResultAllocator &allocator,
	           idx_t &max_token_index, bool preserve_identifier_case_p = true, idx_t starting_token_index = 0)
	    : tokens(tokens), suggestions(suggestions), token_index(starting_token_index), allocator(allocator),
	      max_token_index(max_token_index), preserve_identifier_case(preserve_identifier_case_p) {
	}
	MatchState(MatchState &state)
	    : tokens(state.tokens), suggestions(state.suggestions), token_index(state.token_index),
	      partial_gt(state.partial_gt), allocator(state.allocator), max_token_index(state.max_token_index),
	      preserve_identifier_case(state.preserve_identifier_case) {
	}

	vector<MatcherToken> &tokens;
	vector<MatcherSuggestion> &suggestions;
	reference_set_t<const Matcher> added_suggestions;
	idx_t token_index;
	//! leading '>' chars already consumed from a '>'-run token (nested type closers, e.g. array<array<int>>)
	idx_t partial_gt = 0;
	ParseResultAllocator &allocator;
	idx_t &max_token_index;
	bool preserve_identifier_case = true;

	//! adopt a child state's parse position (token index + partial '>'-run progress)
	void SyncPosition(const MatchState &child) {
		token_index = child.token_index;
		partial_gt = child.partial_gt;
	}

	void UpdateMaxTokenIndex() {
		if (token_index > max_token_index) {
			max_token_index = token_index;
		}
	}

	idx_t GetMaxTokenIndex() const {
		return max_token_index;
	}

	void AddSuggestion(MatcherSuggestion suggestion);
};

enum class MatcherType {
	KEYWORD,
	LIST,
	OPTIONAL,
	CHOICE,
	REPEAT,
	VARIABLE,
	STRING_LITERAL,
	NUMBER_LITERAL,
	OPERATOR,
	END_OF_INPUT
};

class Matcher {
public:
	explicit Matcher(MatcherType type) : type(type) {
	}
	virtual ~Matcher() = default;

	//! Match
	virtual MatchResultType Match(MatchState &state) const = 0;
	virtual optional_ptr<ParseResult> MatchParseResult(MatchState &state) const = 0;
	virtual SuggestionType AddSuggestion(MatchState &state) const;
	virtual SuggestionType AddSuggestionInternal(MatchState &state) const = 0;
	virtual string ToString() const = 0;
	void Print() const;

	MatcherType Type() const {
		return type;
	}
	void SetName(string name_p) {
		name = std::move(name_p);
	}
	string GetName() const;

public:
	template <class TARGET>
	TARGET &Cast() {
		if (type != TARGET::TYPE) {
			throw InternalException("Failed to cast matcher to type - matcher type mismatch");
		}
		return reinterpret_cast<TARGET &>(*this);
	}

	template <class TARGET>
	const TARGET &Cast() const {
		if (type != TARGET::TYPE) {
			throw InternalException("Failed to cast matcher to type - matcher type mismatch");
		}
		return reinterpret_cast<const TARGET &>(*this);
	}

protected:
	MatcherType type;
	string name;
};

class MatcherAllocator {
public:
	Matcher &Allocate(unique_ptr<Matcher> matcher);

private:
	vector<unique_ptr<Matcher>> matchers;
};

class ParseResultAllocator {
public:
	optional_ptr<ParseResult> Allocate(unique_ptr<ParseResult> parse_result);

private:
	vector<unique_ptr<ParseResult>> parse_results;
};

struct PEGMatcher {
	MatcherAllocator allocator;

	Matcher &ProgramMatcher() {
		return *program_matcher;
	}
	Matcher &TopLevelStatementMatcher() {
		return *top_level_statement_matcher;
	}

	static shared_ptr<PEGMatcher> Get(ClientContext &context);
	static shared_ptr<PEGMatcher> Get(DatabaseInstance &db);

private:
	friend struct ParserCache;
	optional_ptr<Matcher> program_matcher;
	optional_ptr<Matcher> top_level_statement_matcher;
};

//! Per-database cache holder for the compiled PEG root matcher and transformer factory.
//! Both are always invalidated together, so they share one mutex and one Invalidate() call.
struct ParserCache {
	//! Process-wide cache for the fork's compiled grammar. The host DatabaseInstance's
	//! ParserCache is a duckdb:: type holding the host's grammar, so the fork keeps its own.
	static ParserCache &GetDefault();

	shared_ptr<PEGMatcher> GetMatcher();
	shared_ptr<PEGTransformerFactory> GetTransformerFactory();
	void Invalidate();

private:
	std::mutex mutex;
	shared_ptr<PEGMatcher> matcher;
	shared_ptr<PEGTransformerFactory> transformer_factory;
};

} // namespace duckdb_fork
