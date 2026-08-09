// SPDX-License-Identifier: MPL-2.0
#pragma once

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

[[nodiscard]] auto validPageName(std::string_view name) -> bool;
[[nodiscard]] auto pageLinks(std::string_view source) -> std::vector<PageLinkOccurrence>;

} // namespace hieda::notebook::authored_text
