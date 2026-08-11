// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "query_parser.hpp"

#include <optional>
#include <string>
#include <vector>

namespace hieda::notebook::query_evaluation {

struct PropertyValue {
    std::string key;
    std::string value;
};

struct Candidate {
    QueryResultRow row;
    std::optional<BlockId> parentId;
    std::vector<PropertyValue> properties;
    std::size_t journalOutlineOrder{0};
};

[[nodiscard]] auto evaluate(const query_language::Query& query,
                            const BlockId& queryEntryId,
                            const std::vector<Candidate>& candidates)
    -> std::vector<QueryResultRow>;

} // namespace hieda::notebook::query_evaluation
