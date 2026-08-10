// SPDX-License-Identifier: MPL-2.0
#include "query_parser.hpp"
#include "authored_text_parser.hpp"

#include <charconv>
#include <chrono>
#include <string>

namespace hieda::notebook::query_language {
namespace {

auto
isSeparator(char character) -> bool
{
    return character == ' ' || character == '\n';
}

auto
trimmedBounds(std::string_view source) -> std::pair<std::size_t, std::size_t>
{
    auto begin = std::size_t{0};
    while (begin < source.size() && isSeparator(source[begin])) {
        ++begin;
    }
    auto end = source.size();
    while (end > begin && isSeparator(source[end - 1])) {
        --end;
    }
    return {begin, end};
}

class Parser {
  public:
    Parser(std::string_view source, std::size_t begin, std::size_t end)
        : source_(source), position_(begin), end_(end)
    {
    }

    auto
    parseQuery() -> ParseResult
    {
        ParseResult result;
        result.hasIntent = true;
        Query query;
        if (!consume("{{query")) {
            fail("Expected the Query opener.", "{{query");
        } else {
            skipSeparators();
            if (!consume("(where") || !requireSeparator()) {
                fail("A Query requires one where clause.", "(where ...)");
            } else {
                skipSeparators();
                if (parsePredicate(query.where)) {
                    skipSeparators();
                    if (!consume(")")) {
                        fail("The where clause has unexpected content.", ")");
                    } else {
                        skipSeparators();
                        if (source_.substr(position_, 9) == "(order-by") {
                            parseOrderBy(query);
                            skipSeparators();
                        }
                        if (!error_ &&
                            source_.substr(position_, 6) == "(limit") {
                            parseLimit(query);
                            skipSeparators();
                        }
                        if (!consume("}}")) {
                            fail("The Query is incomplete.", "}}");
                        } else if (position_ != end_) {
                            fail("Unexpected content follows the Query.",
                                 "end of Query");
                        }
                    }
                }
            }
        }
        if (error_) {
            result.error = std::move(error_);
        } else {
            result.query = std::move(query);
        }
        return result;
    }

  private:
    void
    parseOrderBy(Query& query)
    {
        static_cast<void>(consume("(order-by"));
        const auto key = separatedAtom();
        if (key == "update-time") {
            query.sortKey = SortKey::updateTime;
        } else if (key == "creation-time") {
            query.sortKey = SortKey::creationTime;
        } else if (key == "journal-date") {
            query.sortKey = SortKey::journalDate;
        } else {
            fail("The Query sort key is not supported.",
                 "update-time, creation-time, or journal-date");
            return;
        }
        const auto direction = separatedAtom();
        if (direction == "asc") {
            query.descending = false;
        } else if (direction == "desc") {
            query.descending = true;
        } else {
            fail("The Query sort direction is invalid.", "asc or desc");
            return;
        }
        if (!closePredicate()) {
            return;
        }
    }

    void
    parseLimit(Query& query)
    {
        static_cast<void>(consume("(limit"));
        const auto text = separatedAtom();
        std::uint64_t value = 0;
        const auto [end, error] =
            std::from_chars(text.data(), text.data() + text.size(), value);
        if (text.empty() || error != std::errc{} ||
            end != text.data() + text.size() || value == 0) {
            fail("The Query limit must be a positive integer.",
                 "positive integer");
            return;
        }
        query.limit = value;
        static_cast<void>(closePredicate());
    }

    auto
    parsePredicate(Predicate& predicate) -> bool
    {
        if (!consume("(")) {
            return fail("Expected a Query predicate.", "(predicate ...)");
        }
        const auto name = readAtom();
        if (name == "all") {
            predicate.kind = PredicateKind::all;
            return closePredicate();
        }
        if (name == "and" || name == "or") {
            predicate.kind = name == "and" ? PredicateKind::conjunction
                                           : PredicateKind::disjunction;
            if (!requireSeparator()) {
                return fail("and and or require at least two predicates.",
                            "two or more predicates");
            }
            skipSeparators();
            while (position_ < end_ && source_[position_] == '(') {
                predicate.operands.emplace_back();
                if (!parsePredicate(predicate.operands.back())) {
                    return false;
                }
                skipSeparators();
            }
            if (predicate.operands.size() < 2) {
                return fail("and and or require at least two predicates.",
                            "two or more predicates");
            }
            return consume(")") ||
                   fail("The Boolean predicate has unexpected content.", ")");
        }
        if (name == "not") {
            predicate.kind = PredicateKind::negation;
            if (!requireSeparator()) {
                return fail("not requires exactly one predicate.",
                            "one predicate");
            }
            skipSeparators();
            predicate.operands.emplace_back();
            if (!parsePredicate(predicate.operands.back())) {
                return false;
            }
            skipSeparators();
            return consume(")") ||
                   fail("not requires exactly one predicate.", ")");
        }
        if (name == "type") {
            predicate.kind = PredicateKind::blockType;
            const auto value = separatedAtom();
            if (value == "entry") {
                predicate.blockType = QueryResultBlockType::entry;
            } else if (value == "page") {
                predicate.blockType = QueryResultBlockType::page;
            } else {
                return fail("The Block type is not supported.",
                            "entry or page");
            }
        } else if (name == "page-context") {
            predicate.kind = PredicateKind::pageContext;
            const auto value = separatedAtom();
            if (value == "named") {
                predicate.pageKind = PageKind::named;
            } else if (value == "journal") {
                predicate.pageKind = PageKind::journal;
            } else {
                return fail("The Page kind is not supported.",
                            "named or journal");
            }
        } else if (name == "journal-date") {
            predicate.kind = PredicateKind::journalDate;
            const auto comparison = separatedAtom();
            if (comparison == "=") {
                predicate.dateComparison = JournalDateComparison::equal;
            } else if (comparison == "<") {
                predicate.dateComparison = JournalDateComparison::less;
            } else if (comparison == "<=") {
                predicate.dateComparison = JournalDateComparison::lessOrEqual;
            } else if (comparison == ">") {
                predicate.dateComparison = JournalDateComparison::greater;
            } else if (comparison == ">=") {
                predicate.dateComparison =
                    JournalDateComparison::greaterOrEqual;
            } else {
                return fail("The Journal Date comparison is not supported.",
                            "=, <, <=, >, or >=");
            }
            const auto text = separatedAtom();
            if (!parseDate(text, predicate.journalDate)) {
                return fail("The Journal Date is invalid.", "YYYY-MM-DD");
            }
        } else if (name == "text-contains") {
            predicate.kind = PredicateKind::textContains;
            if (!prepareSeparatedValue() || !parseString(predicate.value) ||
                predicate.value.empty()) {
                return fail("text-contains requires a non-empty valid string.",
                            "non-empty quoted string");
            }
        } else if (name == "property-exists" || name == "property-equals") {
            predicate.kind = name == "property-exists"
                                 ? PredicateKind::propertyExists
                                 : PredicateKind::propertyEquals;
            const auto key = separatedAtom();
            if (!authored_text::validPageName(key)) {
                return fail("The Property key is invalid.", "property key");
            }
            predicate.propertyKey = std::string(key);
            if (name == "property-equals" &&
                (!prepareSeparatedValue() || !parseString(predicate.value))) {
                return fail("property-equals requires a valid string value.",
                            "quoted string");
            }
        } else {
            return fail("This Query predicate is not supported.",
                        "supported predicate");
        }
        return closePredicate();
    }

    auto
    closePredicate() -> bool
    {
        skipSeparators();
        return consume(")") ||
               fail("The predicate has unexpected content.", ")");
    }

    auto
    prepareSeparatedValue() -> bool
    {
        if (!requireSeparator()) {
            return false;
        }
        skipSeparators();
        return true;
    }

    auto
    separatedAtom() -> std::string_view
    {
        if (!prepareSeparatedValue()) {
            return {};
        }
        return readAtom();
    }

    auto
    readAtom() -> std::string_view
    {
        const auto begin = position_;
        while (position_ < end_ && !isSeparator(source_[position_]) &&
               source_[position_] != '(' && source_[position_] != ')') {
            ++position_;
        }
        return source_.substr(begin, position_ - begin);
    }

    auto
    parseString(std::string& value) -> bool
    {
        if (!consume("\"")) {
            return false;
        }
        while (position_ < end_) {
            const auto character = source_[position_++];
            if (character == '"') {
                return true;
            }
            if (character == '\n') {
                return false;
            }
            if (character != '\\') {
                value.push_back(character);
                continue;
            }
            if (position_ >= end_) {
                return false;
            }
            const auto escaped = source_[position_++];
            if (escaped == '"' || escaped == '\\') {
                value.push_back(escaped);
            } else if (escaped == 'n') {
                value.push_back('\n');
            } else {
                return false;
            }
        }
        return false;
    }

    static auto
    parseDate(std::string_view text, JournalDate& date) -> bool
    {
        if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
            return false;
        }
        int year = 0;
        unsigned month = 0;
        unsigned day = 0;
        const auto parsePart = [](std::string_view part, auto& output) -> bool {
            const auto [end, error] =
                std::from_chars(part.data(), part.data() + part.size(), output);
            return error == std::errc{} && end == part.data() + part.size();
        };
        if (!parsePart(text.substr(0, 4), year) || year < 1 || year > 9999 ||
            !parsePart(text.substr(5, 2), month) ||
            !parsePart(text.substr(8, 2), day) ||
            !std::chrono::year_month_day{std::chrono::year{year},
                                         std::chrono::month{month},
                                         std::chrono::day{day}}
                 .ok()) {
            return false;
        }
        date = {year, static_cast<std::uint8_t>(month),
                static_cast<std::uint8_t>(day)};
        return true;
    }

    auto
    consume(std::string_view token) -> bool
    {
        if (source_.substr(position_, token.size()) != token) {
            return false;
        }
        position_ += token.size();
        return true;
    }

    [[nodiscard]] auto
    requireSeparator() const -> bool
    {
        return position_ < end_ && isSeparator(source_[position_]);
    }

    void
    skipSeparators()
    {
        while (position_ < end_ && isSeparator(source_[position_])) {
            ++position_;
        }
    }

    auto
    fail(std::string message, std::string expected) -> bool
    {
        if (!error_) {
            error_ = QueryError{position_, position_ < end_ ? 1U : 0U,
                                std::move(message), std::move(expected)};
        }
        return false;
    }

    std::string_view source_;
    std::size_t position_{0};
    std::size_t end_{0};
    std::optional<QueryError> error_;
};

} // namespace

auto
hasQueryIntent(std::string_view source) -> bool
{
    const auto [begin, end] = trimmedBounds(source);
    constexpr std::string_view opener = "{{query";
    if (end - begin < opener.size() ||
        source.substr(begin, opener.size()) != opener) {
        return false;
    }
    const auto boundary = begin + opener.size();
    return boundary == end || isSeparator(source[boundary]);
}

auto
parse(std::string_view source) -> ParseResult
{
    if (!hasQueryIntent(source)) {
        return {};
    }
    const auto [begin, end] = trimmedBounds(source);
    return Parser(source, begin, end).parseQuery();
}

} // namespace hieda::notebook::query_language
