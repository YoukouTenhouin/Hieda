// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "hieda/notebook/notebook_session.hpp"

#include <optional>
#include <string_view>
#include <vector>

namespace hieda::notebook::query_language {

enum class PredicateKind : std::uint8_t {
    blockType,
    pageContext,
    journalDate,
    textContains,
    propertyExists,
    propertyEquals,
    all,
    conjunction,
    disjunction,
    negation,
    authoredPageLink,
    authoredBlockReference,
    childOf,
    descendantOf,
    parentOf,
    ancestorOf,
    inPageSubtree,
    pageLinksTo,
    blockReferences,
    linkedBy,
    blockReferencedBy,
};
enum class AnchorKind : std::uint8_t { self, pageName, blockId };
enum class JournalDateComparison : std::uint8_t {
    equal,
    less,
    lessOrEqual,
    greater,
    greaterOrEqual,
};

struct Predicate {
    PredicateKind kind{PredicateKind::all};
    QueryResultBlockType blockType{QueryResultBlockType::entry};
    PageKind pageKind{PageKind::named};
    JournalDateComparison dateComparison{JournalDateComparison::equal};
    JournalDate journalDate;
    std::string value;
    std::string propertyKey;
    AnchorKind anchorKind{AnchorKind::self};
    std::string anchorPageName;
    BlockId anchorBlockId;
    std::size_t anchorSourceByteOffset{0};
    std::size_t anchorSourceByteLength{0};
    std::vector<Predicate> operands;
};

enum class SortKey : std::uint8_t {
    updateTime,
    creationTime,
    journalDate,
};

struct Query {
    Predicate where;
    std::optional<SortKey> sortKey;
    bool descending{false};
    std::optional<std::uint64_t> limit;
};

struct ParseResult {
    bool hasIntent{false};
    std::optional<Query> query;
    std::optional<QueryError> error;
};

struct PageAnchorRename {
    std::string_view oldName;
    std::string_view newName;
};

[[nodiscard]] auto hasQueryIntent(std::string_view source) -> bool;
[[nodiscard]] auto parse(std::string_view source) -> ParseResult;
[[nodiscard]] auto rewritePageAnchors(std::string_view source,
                                      PageAnchorRename rename) -> std::string;

} // namespace hieda::notebook::query_language
