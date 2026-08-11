// SPDX-License-Identifier: MPL-2.0
#include "query_evaluator.hpp"
#include "authored_text_parser.hpp"

#include <algorithm>
#include <map>
#include <ranges>
#include <tuple>
#include <utility>

namespace hieda::notebook::query_evaluation {
namespace {

auto
dateKey(JournalDate date)
{
    return std::tuple{date.year, date.month, date.day};
}

struct BlockIdLess {
    auto
    operator()(const BlockId& left, const BlockId& right) const -> bool
    {
        return std::ranges::lexicographical_compare(left.bytes, right.bytes);
    }
};

class Evaluator {
  public:
    Evaluator(const BlockId& queryEntryId,
              const std::vector<Candidate>& candidates)
        : queryEntryId_(queryEntryId), candidates_(candidates)
    {
        for (const auto& candidate : candidates_) {
            byId_.emplace(candidate.row.metadata.id, &candidate);
            if (candidate.row.type == QueryResultBlockType::page &&
                candidate.row.pageKind == PageKind::named) {
                pagesByName_.emplace(candidate.row.pageName, &candidate);
            }
        }
    }

    [[nodiscard]] auto
    matches(const Candidate& candidate,
            const query_language::Predicate& predicate) const -> bool
    {
        using query_language::PredicateKind;
        if (predicate.kind == PredicateKind::all) {
            return true;
        }
        if (predicate.kind == PredicateKind::conjunction) {
            return std::ranges::all_of(predicate.operands,
                                       [&](const auto& operand) -> bool {
                                           return matches(candidate, operand);
                                       });
        }
        if (predicate.kind == PredicateKind::disjunction) {
            return std::ranges::any_of(predicate.operands,
                                       [&](const auto& operand) -> bool {
                                           return matches(candidate, operand);
                                       });
        }
        if (predicate.kind == PredicateKind::negation) {
            return !matches(candidate, predicate.operands.front());
        }
        if (predicate.kind == PredicateKind::blockType) {
            return candidate.row.type == predicate.blockType;
        }
        if (predicate.kind == PredicateKind::pageContext) {
            return candidate.row.pageKind == predicate.pageKind;
        }
        if (predicate.kind == PredicateKind::journalDate) {
            return matchesJournalDate(candidate, predicate);
        }
        if (predicate.kind == PredicateKind::textContains) {
            return isEntry(candidate) &&
                   candidate.row.authoredText.find(predicate.value) !=
                       std::string::npos;
        }
        if (predicate.kind == PredicateKind::inPageSubtree) {
            const auto* pageAnchor = pageNameAnchor(predicate);
            return pageAnchor != nullptr &&
                   matchesPageSubtree(candidate, pageAnchor->name);
        }
        if (predicate.kind == PredicateKind::childOf ||
            predicate.kind == PredicateKind::descendantOf ||
            predicate.kind == PredicateKind::parentOf ||
            predicate.kind == PredicateKind::ancestorOf) {
            return matchesContainment(candidate, predicate);
        }
        if (predicate.kind == PredicateKind::pageLinksTo ||
            predicate.kind == PredicateKind::blockReferences ||
            predicate.kind == PredicateKind::linkedBy ||
            predicate.kind == PredicateKind::blockReferencedBy) {
            return matchesResolvedReference(candidate, predicate);
        }
        if (!isEntry(candidate)) {
            return false;
        }
        if (predicate.kind == PredicateKind::authoredPageLink) {
            const auto* pageAnchor = pageNameAnchor(predicate);
            return pageAnchor != nullptr &&
                   std::ranges::any_of(pageLinkNames(candidate),
                                       [&](const auto& name) -> bool {
                                           return name == pageAnchor->name;
                                       });
        }
        if (predicate.kind == PredicateKind::authoredBlockReference) {
            const auto* blockId = blockAnchor(predicate);
            return blockId != nullptr &&
                   entryBlockReferences(candidate, *blockId);
        }
        if (predicate.kind == PredicateKind::propertyExists) {
            return std::ranges::any_of(
                candidate.properties, [&](const auto& property) -> bool {
                    return property.key == predicate.propertyKey;
                });
        }
        if (predicate.kind == PredicateKind::propertyEquals) {
            return std::ranges::any_of(
                candidate.properties, [&](const auto& property) -> bool {
                    return property.key == predicate.propertyKey &&
                           property.value == predicate.value;
                });
        }
        return false;
    }

  private:
    [[nodiscard]] static auto
    isEntry(const Candidate& candidate) -> bool
    {
        return candidate.row.type == QueryResultBlockType::entry;
    }

    [[nodiscard]] static auto
    pageNameAnchor(const query_language::Predicate& predicate)
        -> const query_language::PageNameAnchor*
    {
        return predicate.anchor ? std::get_if<query_language::PageNameAnchor>(
                                      &predicate.anchor->target)
                                : nullptr;
    }

    [[nodiscard]] static auto
    blockAnchor(const query_language::Predicate& predicate) -> const BlockId*
    {
        return predicate.anchor
                   ? std::get_if<BlockId>(&predicate.anchor->target)
                   : nullptr;
    }

    [[nodiscard]] auto
    resolveAnchor(const query_language::Predicate& predicate) const
        -> const Candidate*
    {
        if (!predicate.anchor) {
            return nullptr;
        }
        if (std::holds_alternative<query_language::SelfAnchor>(
                predicate.anchor->target)) {
            return findById(queryEntryId_);
        }
        if (const auto* blockId = blockAnchor(predicate)) {
            return findById(*blockId);
        }
        const auto* pageAnchor = pageNameAnchor(predicate);
        const auto found = pageAnchor != nullptr
                               ? pagesByName_.find(pageAnchor->name)
                               : pagesByName_.end();
        return found == pagesByName_.end() ? nullptr : found->second;
    }

    [[nodiscard]] auto
    findById(const BlockId& blockId) const -> const Candidate*
    {
        const auto found = byId_.find(blockId);
        return found == byId_.end() ? nullptr : found->second;
    }

    [[nodiscard]] static auto
    matchesJournalDate(const Candidate& candidate,
                       const query_language::Predicate& predicate) -> bool
    {
        if (!candidate.row.journalDate) {
            return false;
        }
        const auto actual = dateKey(*candidate.row.journalDate);
        const auto expected = dateKey(predicate.journalDate);
        switch (predicate.dateComparison) {
        case query_language::JournalDateComparison::equal:
            return actual == expected;
        case query_language::JournalDateComparison::less:
            return actual < expected;
        case query_language::JournalDateComparison::lessOrEqual:
            return actual <= expected;
        case query_language::JournalDateComparison::greater:
            return actual > expected;
        case query_language::JournalDateComparison::greaterOrEqual:
            return actual >= expected;
        }
        return false;
    }

    [[nodiscard]] static auto
    matchesPageSubtree(const Candidate& candidate, std::string_view root)
        -> bool
    {
        const auto& candidateName = candidate.row.pageName;
        return candidate.row.pageKind == PageKind::named &&
               (candidateName == root || (candidateName.starts_with(root) &&
                                          candidateName.size() > root.size() &&
                                          candidateName[root.size()] == '/'));
    }

    [[nodiscard]] auto
    hasAncestor(const Candidate& descendant, const BlockId& ancestorId) const
        -> bool
    {
        const auto* current = &descendant;
        for (std::size_t depth = 0;
             current->parentId && depth < candidates_.size(); ++depth) {
            if (*current->parentId == ancestorId) {
                return true;
            }
            current = findById(*current->parentId);
            if (current == nullptr) {
                return false;
            }
        }
        return false;
    }

    [[nodiscard]] auto
    matchesContainment(const Candidate& candidate,
                       const query_language::Predicate& predicate) const -> bool
    {
        using query_language::PredicateKind;
        const auto* anchored = resolveAnchor(predicate);
        if (anchored == nullptr) {
            return false;
        }
        if (predicate.kind == PredicateKind::childOf) {
            return candidate.parentId &&
                   *candidate.parentId == anchored->row.metadata.id;
        }
        if (predicate.kind == PredicateKind::descendantOf) {
            return hasAncestor(candidate, anchored->row.metadata.id);
        }
        if (predicate.kind == PredicateKind::parentOf) {
            return anchored->parentId &&
                   *anchored->parentId == candidate.row.metadata.id;
        }
        return hasAncestor(*anchored, candidate.row.metadata.id);
    }

    [[nodiscard]] auto
    pageLinkNames(const Candidate& source) const
        -> const std::vector<std::string>&
    {
        auto found = pageLinkNamesBySource_.find(source.row.metadata.id);
        if (found != pageLinkNamesBySource_.end()) {
            return found->second;
        }
        auto occurrences = authored_text::pageLinks(source.row.authoredText);
        std::vector<std::string> names;
        names.reserve(occurrences.size());
        for (auto& occurrence : occurrences) {
            names.push_back(std::move(occurrence.pageName));
        }
        return pageLinkNamesBySource_
            .emplace(source.row.metadata.id, std::move(names))
            .first->second;
    }

    [[nodiscard]] auto
    blockReferenceTargets(const Candidate& source) const
        -> const std::vector<BlockId>&
    {
        auto found =
            blockReferenceTargetsBySource_.find(source.row.metadata.id);
        if (found != blockReferenceTargetsBySource_.end()) {
            return found->second;
        }
        auto occurrences =
            authored_text::blockReferences(source.row.authoredText);
        std::vector<BlockId> targets;
        targets.reserve(occurrences.size());
        for (const auto& occurrence : occurrences) {
            targets.push_back(occurrence.targetId);
        }
        return blockReferenceTargetsBySource_
            .emplace(source.row.metadata.id, std::move(targets))
            .first->second;
    }

    [[nodiscard]] auto
    entryPageLinksTo(const Candidate& source, const BlockId& targetId) const
        -> bool
    {
        if (!isEntry(source)) {
            return false;
        }
        return std::ranges::any_of(
            pageLinkNames(source), [&](const auto& pageName) -> bool {
                const auto target = pagesByName_.find(pageName);
                return target != pagesByName_.end() &&
                       target->second->row.metadata.id == targetId;
            });
    }

    [[nodiscard]] auto
    entryBlockReferences(const Candidate& source, const BlockId& targetId) const
        -> bool
    {
        return isEntry(source) &&
               std::ranges::any_of(blockReferenceTargets(source),
                                   [&](const auto& referencedId) -> bool {
                                       return referencedId == targetId;
                                   });
    }

    [[nodiscard]] auto
    matchesResolvedReference(const Candidate& candidate,
                             const query_language::Predicate& predicate) const
        -> bool
    {
        using query_language::PredicateKind;
        const auto* anchored = resolveAnchor(predicate);
        if (anchored == nullptr) {
            return false;
        }
        if (predicate.kind == PredicateKind::pageLinksTo) {
            return anchored->row.type == QueryResultBlockType::page &&
                   entryPageLinksTo(candidate, anchored->row.metadata.id);
        }
        if (predicate.kind == PredicateKind::blockReferences) {
            return entryBlockReferences(candidate, anchored->row.metadata.id);
        }
        if (predicate.kind == PredicateKind::linkedBy) {
            return entryPageLinksTo(*anchored, candidate.row.metadata.id);
        }
        return entryBlockReferences(*anchored, candidate.row.metadata.id);
    }

    BlockId queryEntryId_;
    const std::vector<Candidate>& candidates_;
    std::map<BlockId, const Candidate*, BlockIdLess> byId_;
    std::map<std::string, const Candidate*> pagesByName_;
    mutable std::map<BlockId, std::vector<std::string>, BlockIdLess>
        pageLinkNamesBySource_;
    mutable std::map<BlockId, std::vector<BlockId>, BlockIdLess>
        blockReferenceTargetsBySource_;
};

} // namespace

auto
evaluate(const query_language::Query& query, const BlockId& queryEntryId,
         const std::vector<Candidate>& candidates)
    -> std::vector<QueryResultRow>
{
    const Evaluator evaluator(queryEntryId, candidates);
    std::vector<const Candidate*> matches;
    for (const auto& candidate : candidates) {
        if (evaluator.matches(candidate, query.where)) {
            matches.push_back(&candidate);
        }
    }
    const auto sortKey =
        query.sortKey.value_or(query_language::SortKey::updateTime);
    const auto descending = query.sortKey ? query.descending : true;
    std::ranges::sort(
        matches, [&](const Candidate* left, const Candidate* right) -> bool {
            const auto identifierLess = [&]() -> bool {
                return std::ranges::lexicographical_compare(
                    left->row.metadata.id.bytes, right->row.metadata.id.bytes);
            };
            if (sortKey == query_language::SortKey::journalDate) {
                const auto leftJournal = left->row.journalDate.has_value();
                const auto rightJournal = right->row.journalDate.has_value();
                if (leftJournal != rightJournal) {
                    return leftJournal;
                }
                if (!leftJournal) {
                    return identifierLess();
                }
                const auto leftDate = dateKey(*left->row.journalDate);
                const auto rightDate = dateKey(*right->row.journalDate);
                if (leftDate != rightDate) {
                    return descending ? leftDate > rightDate
                                      : leftDate < rightDate;
                }
                return left->journalOutlineOrder < right->journalOutlineOrder;
            }
            const auto leftTime =
                sortKey == query_language::SortKey::creationTime
                    ? left->row.metadata.createdAt
                    : left->row.metadata.updatedAt;
            const auto rightTime =
                sortKey == query_language::SortKey::creationTime
                    ? right->row.metadata.createdAt
                    : right->row.metadata.updatedAt;
            if (leftTime == rightTime) {
                return identifierLess();
            }
            return descending ? leftTime > rightTime : leftTime < rightTime;
        });
    std::vector<QueryResultRow> rows;
    rows.reserve(matches.size());
    for (const auto* candidate : matches) {
        rows.push_back(candidate->row);
    }
    return rows;
}

} // namespace hieda::notebook::query_evaluation
