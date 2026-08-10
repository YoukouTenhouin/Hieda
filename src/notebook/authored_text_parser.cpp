// SPDX-License-Identifier: MPL-2.0
#include "authored_text_parser.hpp"
#include "query_parser.hpp"

#include <algorithm>
#include <charconv>
#include <ranges>

namespace hieda::notebook::authored_text {

auto
validPageName(std::string_view name) -> bool
{
    if (name.empty() || name.size() > 255) {
        return false;
    }
    std::size_t segmentStart = 0;
    while (segmentStart < name.size()) {
        const auto separator = name.find('/', segmentStart);
        const auto segmentEnd =
            separator == std::string_view::npos ? name.size() : separator;
        const auto segment =
            name.substr(segmentStart, segmentEnd - segmentStart);
        if (segment.empty() || segment.size() > 64 || segment.front() < 'a' ||
            segment.front() > 'z' ||
            !std::ranges::all_of(segment, [](char character) -> bool {
                return (character >= 'a' && character <= 'z') ||
                       (character >= '0' && character <= '9') ||
                       character == '_' || character == '-';
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

auto
propertyDelimiter(std::string_view line, std::size_t start) -> std::size_t
{
    const auto delimiter = line.find("::", start);
    return delimiter != std::string_view::npos &&
                   validPageName(line.substr(start, delimiter - start))
               ? delimiter
               : std::string_view::npos;
}

auto
parseUuid(std::string_view value) -> std::optional<BlockId>
{
    if (value.size() != 36) {
        return std::nullopt;
    }
    BlockId identifier;
    std::size_t byteIndex = 0;
    for (std::size_t index = 0; index < value.size();) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (value[index++] != '-') {
                return std::nullopt;
            }
            continue;
        }
        unsigned int byte = 0;
        const auto* begin = value.data() + index;
        const auto* end = begin + 2;
        const auto [position, error] = std::from_chars(begin, end, byte, 16);
        if (error != std::errc{} || position != end ||
            byteIndex >= identifier.bytes.size()) {
            return std::nullopt;
        }
        identifier.bytes[byteIndex++] = static_cast<std::byte>(byte);
        index += 2;
    }
    return byteIndex == identifier.bytes.size() ? std::optional{identifier}
                                                : std::nullopt;
}

void
scanInline(std::string_view source, std::size_t begin, std::size_t end,
           std::vector<PageLinkOccurrence>* pageLinks,
           std::vector<BlockReferenceOccurrence>* blockReferences)
{
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
                    index = closer == std::string_view::npos || closer >= end
                                ? end
                                : closer + 2;
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
        if (pageLinks != nullptr && validPageName(body)) {
            pageLinks->push_back(
                {index, closer + 2 - index, std::string(body)});
        }
        constexpr std::string_view blockPrefix = "block:";
        const auto uuid = body.starts_with(blockPrefix)
                              ? body.substr(blockPrefix.size())
                              : std::string_view{};
        if (blockReferences != nullptr) {
            if (const auto targetId = parseUuid(uuid)) {
                blockReferences->push_back(
                    {index, closer + 2 - index, *targetId});
            }
        }
        index = closer + 2;
    }
}

void
scan(std::string_view source, std::vector<PageLinkOccurrence>* pageLinks,
     std::vector<BlockReferenceOccurrence>* blockReferences)
{
    if (query_language::hasQueryIntent(source)) {
        return;
    }
    for (std::size_t lineStart = 0; lineStart <= source.size();) {
        const auto newline = source.find('\n', lineStart);
        const auto lineEnd =
            newline == std::string_view::npos ? source.size() : newline;
        const auto line = source.substr(lineStart, lineEnd - lineStart);
        const auto property = propertyDelimiter(line, 0);
        const auto escapedProperty = line.starts_with('\\')
                                         ? propertyDelimiter(line, 1)
                                         : std::string_view::npos;
        if (property == std::string_view::npos) {
            const auto inlineStart = escapedProperty == std::string_view::npos
                                         ? lineStart
                                         : lineStart + escapedProperty + 2;
            scanInline(source, inlineStart, lineEnd, pageLinks,
                       blockReferences);
        }
        if (newline == std::string_view::npos) {
            break;
        }
        lineStart = newline + 1;
    }
}

} // namespace

auto
pageLinks(std::string_view source) -> std::vector<PageLinkOccurrence>
{
    std::vector<PageLinkOccurrence> links;
    scan(source, &links, nullptr);
    return links;
}

auto
blockReferences(std::string_view source)
    -> std::vector<BlockReferenceOccurrence>
{
    std::vector<BlockReferenceOccurrence> references;
    scan(source, nullptr, &references);
    return references;
}

auto
properties(std::string_view source) -> std::vector<Property>
{
    std::vector<Property> result;
    if (query_language::hasQueryIntent(source)) {
        return result;
    }
    for (std::size_t lineStart = 0; lineStart <= source.size();) {
        const auto newline = source.find('\n', lineStart);
        const auto lineEnd =
            newline == std::string_view::npos ? source.size() : newline;
        const auto line = source.substr(lineStart, lineEnd - lineStart);
        const auto delimiter = propertyDelimiter(line, 0);
        if (delimiter != std::string_view::npos) {
            result.push_back({lineStart, line.size(),
                              std::string(line.substr(0, delimiter)),
                              std::string(line.substr(delimiter + 2))});
        }
        if (newline == std::string_view::npos) {
            break;
        }
        lineStart = newline + 1;
    }
    return result;
}

} // namespace hieda::notebook::authored_text
