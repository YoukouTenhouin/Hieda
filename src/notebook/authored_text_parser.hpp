// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "hieda/notebook/notebook_session.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace hieda::notebook::authored_text {

struct PageLinkOccurrence {
    std::size_t sourceByteOffset{0};
    std::size_t sourceByteLength{0};
    std::string pageName;
};

struct BlockReferenceOccurrence {
    std::size_t sourceByteOffset{0};
    std::size_t sourceByteLength{0};
    BlockId targetId;
};

[[nodiscard]] auto validPageName(std::string_view name) -> bool;
[[nodiscard]] auto pageLinks(std::string_view source)
    -> std::vector<PageLinkOccurrence>;
[[nodiscard]] auto blockReferences(std::string_view source)
    -> std::vector<BlockReferenceOccurrence>;

} // namespace hieda::notebook::authored_text
