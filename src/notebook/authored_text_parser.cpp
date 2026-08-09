// SPDX-License-Identifier: MPL-2.0
#include "authored_text_parser.hpp"

#include <algorithm>
#include <ranges>

namespace hieda::notebook::authored_text {

auto validPageName(std::string_view name) -> bool {
    if (name.empty() || name.size() > 255) {
        return false;
    }
    std::size_t segmentStart = 0;
    while (segmentStart < name.size()) {
        const auto separator = name.find('/', segmentStart);
        const auto segmentEnd = separator == std::string_view::npos ? name.size() : separator;
        const auto segment = name.substr(segmentStart, segmentEnd - segmentStart);
        if (segment.empty() || segment.size() > 64 || segment.front() < 'a' ||
            segment.front() > 'z' || !std::ranges::all_of(segment, [](char character) -> bool {
                return (character >= 'a' && character <= 'z') ||
                       (character >= '0' && character <= '9') || character == '_' ||
                       character == '-';
            })) {
            return false;
        }
        if (separator == std::string_view::npos) {
            return true;
        }
        segmentStart = separator + 1;
    }
    return false;
}

namespace {

auto propertyDelimiter(std::string_view line, std::size_t start) -> std::size_t {
    const auto delimiter = line.find("::", start);
    return delimiter != std::string_view::npos &&
                   validPageName(line.substr(start, delimiter - start))
               ? delimiter
               : std::string_view::npos;
}

void scanInline(std::string_view source, std::size_t begin, std::size_t end,
                std::vector<PageLinkOccurrence>& links) {
    auto index = begin;
    while (index < end) {
        if (source[index] == '\\') {
            auto runEnd = index;
            while (runEnd < end && source[runEnd] == '\\') {
                ++runEnd;
            }
            if (runEnd + 1 < end && source.substr(runEnd, 2) == "[[") {
                if ((runEnd - index) % 2 == 1) {
                    const auto closer = source.find("]]", runEnd + 2);
                    index = closer == std::string_view::npos || closer >= end ? end : closer + 2;
                    continue;
                }
                index = runEnd;
            } else {
                index = runEnd;
                continue;
            }
        }
        if (index + 1 >= end || source.substr(index, 2) != "[[") {
            ++index;
            continue;
        }
        const auto closer = source.find("]]", index + 2);
        if (closer == std::string_view::npos || closer >= end) {
            break;
        }
        const auto body = source.substr(index + 2, closer - index - 2);
        if (validPageName(body)) {
            links.push_back({index, closer + 2 - index, std::string(body)});
        }
        index = closer + 2;
    }
}

} // namespace

auto pageLinks(std::string_view source) -> std::vector<PageLinkOccurrence> {
    std::vector<PageLinkOccurrence> links;
    for (std::size_t lineStart = 0; lineStart <= source.size();) {
        const auto newline = source.find('\n', lineStart);
        const auto lineEnd = newline == std::string_view::npos ? source.size() : newline;
        const auto line = source.substr(lineStart, lineEnd - lineStart);
        const auto property = propertyDelimiter(line, 0);
        const auto escapedProperty =
            line.starts_with('\\') ? propertyDelimiter(line, 1) : std::string_view::npos;
        if (property == std::string_view::npos) {
            const auto inlineStart = escapedProperty == std::string_view::npos
                                         ? lineStart
                                         : lineStart + escapedProperty + 2;
            scanInline(source, inlineStart, lineEnd, links);
        }
        if (newline == std::string_view::npos) {
            break;
        }
        lineStart = newline + 1;
    }
    return links;
}

} // namespace hieda::notebook::authored_text
