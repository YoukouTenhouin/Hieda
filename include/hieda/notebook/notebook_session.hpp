// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace hieda::notebook {

struct NotebookId {
    std::array<std::byte, 16> bytes{};

    auto operator==(const NotebookId&) const -> bool = default;
    [[nodiscard]] auto toString() const -> std::string;
};

struct NotebookInfo {
    NotebookId id;
    std::filesystem::path path;
    std::uint32_t schemaVersion{0};
    std::uint64_t revision{0};

    auto operator==(const NotebookInfo&) const -> bool = default;
};

class NotebookSubscription {
  public:
    NotebookSubscription();
    ~NotebookSubscription();
    NotebookSubscription(NotebookSubscription&&) noexcept;
    auto operator=(NotebookSubscription&&) noexcept -> NotebookSubscription&;
    NotebookSubscription(const NotebookSubscription&) = delete;
    auto operator=(const NotebookSubscription&) -> NotebookSubscription& = delete;

  private:
    friend class NotebookSession;
    class Impl;
    explicit NotebookSubscription(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

struct BlockId {
    std::array<std::byte, 16> bytes{};

    auto operator==(const BlockId&) const -> bool = default;
    [[nodiscard]] auto toString() const -> std::string;
};

struct JournalDate {
    std::int32_t year{0};
    std::uint8_t month{0};
    std::uint8_t day{0};

    auto operator==(const JournalDate&) const -> bool = default;
};

using BlockTimestamp = std::chrono::sys_time<std::chrono::microseconds>;

struct BlockMetadata {
    BlockId id;
    BlockTimestamp createdAt;
    BlockTimestamp updatedAt;

    auto operator==(const BlockMetadata&) const -> bool = default;
};

struct Entry {
    BlockMetadata metadata;
    std::string authoredText;
    std::optional<BlockId> parentEntry;

    auto operator==(const Entry&) const -> bool = default;
};

struct JournalPage {
    JournalDate date;
    std::optional<BlockMetadata> metadata;
    std::vector<Entry> entries;

    auto operator==(const JournalPage&) const -> bool = default;
};

struct PageSummary {
    BlockMetadata metadata;
    std::string name;
    std::string displayTitle;

    auto operator==(const PageSummary&) const -> bool = default;
};

struct Page {
    BlockMetadata metadata;
    std::string name;
    std::string displayTitle;
    std::vector<Entry> entries;

    auto operator==(const Page&) const -> bool = default;
};

struct PageHierarchyNode {
    std::string name;
    std::string localSegment;
    std::optional<PageSummary> page;
    bool hasChildren{false};

    auto operator==(const PageHierarchyNode&) const -> bool = default;
};

struct PageHierarchyBatch {
    std::vector<PageHierarchyNode> nodes;
    std::optional<std::string> continuationCursor;

    auto operator==(const PageHierarchyBatch&) const -> bool = default;
};

enum class PageKind : std::uint8_t { named, journal };

struct OutlinePage {
    PageKind kind{PageKind::named};
    std::optional<BlockMetadata> metadata;
    std::optional<JournalDate> journalDate;
    std::string name;
    std::string displayTitle;
    std::vector<Entry> entries;

    auto operator==(const OutlinePage&) const -> bool = default;
};

struct EditCapabilities {
    bool canUndo{false};
    bool canRedo{false};

    auto operator==(const EditCapabilities&) const -> bool = default;
};

enum class EntryMove : std::uint8_t { indent, outdent, up, down };

using PageAddress = std::variant<BlockId, JournalDate>;

enum class NotebookErrorCode : std::uint8_t {
    pathNotFound,
    pathExists,
    invalidPath,
    invalidNotebook,
    unsupportedVersion,
    alreadyOpen,
    alreadyInUse,
    permissionDenied,
    ioFailure,
    notebookNotOpen,
    invalidJournalDate,
    invalidAuthoredText,
    blockNotFound,
    invalidInsertionPoint,
    invalidCursorPosition,
    invalidStructuralMove,
    blockHasChildren,
    undoUnavailable,
    redoUnavailable,
    invalidPageName,
    invalidPageTitle,
    pageNameConflict,
    pageNotFound,
    staleHierarchyCursor,
};

struct NotebookError {
    NotebookErrorCode code;
    std::filesystem::path path;
    std::string detail;
};

class NotebookException : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

template <typename T> class Result {
  public:
    [[nodiscard]] static auto success(T value) -> Result {
        return Result(std::move(value));
    }
    [[nodiscard]] static auto failure(NotebookError error) -> Result {
        return Result(std::move(error));
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return std::holds_alternative<T>(value_);
    }
    [[nodiscard]] auto value() const& -> const T& {
        return std::get<T>(value_);
    }
    [[nodiscard]] auto value() && -> T {
        return std::get<T>(std::move(value_));
    }
    [[nodiscard]] auto error() const& -> const NotebookError& {
        return std::get<NotebookError>(value_);
    }

  private:
    explicit Result(T value) : value_(std::move(value)) {}
    explicit Result(NotebookError error) : value_(std::move(error)) {}

    std::variant<T, NotebookError> value_;
};

class NotebookSession {
  public:
    NotebookSession();
    ~NotebookSession();

    NotebookSession(const NotebookSession&) = delete;
    auto operator=(const NotebookSession&) -> NotebookSession& = delete;
    NotebookSession(NotebookSession&&) = delete;
    auto operator=(NotebookSession&&) -> NotebookSession& = delete;

    [[nodiscard]] auto create(const std::filesystem::path& path) -> Result<NotebookInfo>;
    [[nodiscard]] auto open(const std::filesystem::path& path) -> Result<NotebookInfo>;
    void close() noexcept;
    [[nodiscard]] auto isOpen() const noexcept -> bool;
    [[nodiscard]] auto current() const -> std::optional<NotebookInfo>;
    [[nodiscard]] auto pages() const -> Result<std::vector<PageSummary>>;
    [[nodiscard]] auto pageHierarchyNode(std::string name) const
        -> Result<std::optional<PageHierarchyNode>>;
    [[nodiscard]] auto
    pageHierarchyChildren(std::optional<std::string> parentName = std::nullopt,
                          std::optional<std::string> continuationCursor = std::nullopt) const
        -> Result<PageHierarchyBatch>;
    [[nodiscard]] auto createPage(std::string name, std::string displayTitle) -> Result<Page>;
    [[nodiscard]] auto deletePage(BlockId pageId) -> Result<Page>;
    [[nodiscard]] auto renamePage(BlockId pageId, std::string name, std::string displayTitle)
        -> Result<Page>;
    [[nodiscard]] auto outline(PageAddress address) const -> Result<OutlinePage>;
    [[nodiscard]] auto insertEntry(PageAddress address, std::optional<BlockId> afterEntry,
                                   std::string authoredText) -> Result<OutlinePage>;
    [[nodiscard]] auto updateEntry(BlockId entryId, std::string authoredText) -> Result<Entry>;
    [[nodiscard]] auto splitEntry(BlockId entryId, std::string authoredText,
                                  std::size_t cursorByteOffset) -> Result<OutlinePage>;
    [[nodiscard]] auto joinEntry(BlockId entryId, std::string authoredText) -> Result<OutlinePage>;
    [[nodiscard]] auto moveEntry(BlockId entryId, EntryMove movement, std::string authoredText)
        -> Result<OutlinePage>;
    [[nodiscard]] auto moveEntryToPage(BlockId entryId, PageAddress destination,
                                       std::optional<BlockId> afterEntry)
        -> Result<std::vector<OutlinePage>>;
    [[nodiscard]] auto deleteEntry(BlockId entryId) -> Result<OutlinePage>;
    [[nodiscard]] auto deleteSubtrees(std::vector<BlockId> entryIds) -> Result<OutlinePage>;
    [[nodiscard]] auto editCapabilities() const -> Result<EditCapabilities>;
    [[nodiscard]] auto undoEdit() -> Result<std::vector<OutlinePage>>;
    [[nodiscard]] auto redoEdit() -> Result<std::vector<OutlinePage>>;
    [[nodiscard]] auto subscribeToChanges(std::function<void()> callback) -> NotebookSubscription;

  private:
    friend class NotebookSessionTestAccess;
    [[nodiscard]] auto journalPage(JournalDate date) const -> Result<JournalPage>;
    [[nodiscard]] auto page(BlockId pageId) const -> Result<Page>;
    [[nodiscard]] auto insertPageEntry(BlockId pageId, std::optional<BlockId> afterEntry,
                                       std::string authoredText) -> Result<Page>;
    [[nodiscard]] auto updatePageEntry(BlockId entryId, std::string authoredText) -> Result<Entry>;
    [[nodiscard]] auto splitPageEntry(BlockId entryId, std::string authoredText,
                                      std::size_t cursorByteOffset) -> Result<Page>;
    [[nodiscard]] auto joinPageEntry(BlockId entryId, std::string authoredText) -> Result<Page>;
    [[nodiscard]] auto movePageEntry(BlockId entryId, EntryMove movement, std::string authoredText)
        -> Result<Page>;
    [[nodiscard]] auto deletePageEntry(BlockId entryId) -> Result<Page>;
    [[nodiscard]] auto deletePageSubtrees(std::vector<BlockId> entryIds) -> Result<Page>;
    [[nodiscard]] auto insertJournalEntry(JournalDate date, std::optional<BlockId> afterEntry,
                                          std::string authoredText) -> Result<JournalPage>;
    [[nodiscard]] auto updateJournalEntry(BlockId entryId, std::string authoredText)
        -> Result<Entry>;
    [[nodiscard]] auto splitJournalEntry(BlockId entryId, std::string authoredText,
                                         std::size_t cursorByteOffset) -> Result<JournalPage>;
    [[nodiscard]] auto joinJournalEntry(BlockId entryId, std::string authoredText)
        -> Result<JournalPage>;
    [[nodiscard]] auto moveJournalEntry(BlockId entryId, EntryMove movement,
                                        std::string authoredText) -> Result<JournalPage>;
    [[nodiscard]] auto deleteJournalEntry(BlockId entryId) -> Result<JournalPage>;
    [[nodiscard]] auto deleteJournalSubtrees(std::vector<BlockId> entryIds) -> Result<JournalPage>;
    [[nodiscard]] auto entryPageKind(BlockId entryId) const -> Result<PageKind>;
    auto runPageCommand(std::function<Result<Page>()> command) -> Result<Page>;
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hieda::notebook
