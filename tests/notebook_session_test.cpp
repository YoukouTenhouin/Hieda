// SPDX-License-Identifier: MPL-2.0
#include "hieda/notebook/notebook_session.hpp"
#include "notebook_session_test_access.hpp"

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef _WIN32
#    include <unistd.h>
#endif

namespace {

class TemporaryDirectory {
  public:
    TemporaryDirectory()
    {
        const auto suffix = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        path_ =
            std::filesystem::temp_directory_path() / ("hieda-test-" + suffix);
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory()
    {
        std::filesystem::remove_all(path_);
    }

    [[nodiscard]] auto
    path() const -> const std::filesystem::path&
    {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

class TimestampOverride {
  public:
    explicit TimestampOverride(hieda::notebook::BlockTimestamp timestamp)
    {
        hieda::notebook::NotebookSessionTestAccess::setCurrentTimestamp(
            timestamp);
    }

    ~TimestampOverride()
    {
        hieda::notebook::NotebookSessionTestAccess::setCurrentTimestamp(
            std::nullopt);
    }
};

auto
lmdbFixturePath(const std::filesystem::path& path) -> std::string
{
#ifdef _WIN32
    const auto utf8 = path.u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
#else
    return path.native();
#endif
}

void
appendU16(std::vector<std::uint8_t>& output, std::uint16_t value)
{
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
    output.push_back(static_cast<std::uint8_t>(value));
}

void
appendU32(std::vector<std::uint8_t>& output, std::uint32_t value)
{
    for (int shift = 24; shift >= 0; shift -= 8) {
        output.push_back(
            static_cast<std::uint8_t>(value >> static_cast<unsigned>(shift)));
    }
}

void
appendField(std::vector<std::uint8_t>& output, std::uint16_t tag,
            const std::vector<std::uint8_t>& value)
{
    appendU16(output, tag);
    appendU32(output, static_cast<std::uint32_t>(value.size()));
    output.insert(output.end(), value.begin(), value.end());
}

void
createNotebookFixture(const std::filesystem::path& path,
                      std::uint32_t fixtureSchemaVersion,
                      bool includeIdentity = true)
{
    MDB_env* environment = nullptr;
    REQUIRE(mdb_env_create(&environment) == MDB_SUCCESS);
    REQUIRE(mdb_env_set_maxdbs(environment, 1) == MDB_SUCCESS);
    const auto encodedPath = lmdbFixturePath(path);
    REQUIRE(mdb_env_open(environment, encodedPath.c_str(), MDB_NOSUBDIR,
                         0600) == MDB_SUCCESS);
    MDB_txn* transaction = nullptr;
    REQUIRE(mdb_txn_begin(environment, nullptr, 0, &transaction) ==
            MDB_SUCCESS);
    MDB_dbi metadata = 0;
    REQUIRE(mdb_dbi_open(transaction, "metadata", MDB_CREATE, &metadata) ==
            MDB_SUCCESS);

    std::vector<std::uint8_t> manifest;
    appendU16(manifest, 1);
    const std::string_view magic = "HIEDA_NOTEBOOK";
    appendField(manifest, 1,
                std::vector<std::uint8_t>(magic.begin(), magic.end()));
    std::vector<std::uint8_t> number;
    appendU32(number, 1);
    appendField(manifest, 2, number);
    number.clear();
    appendU32(number, fixtureSchemaVersion);
    appendField(manifest, 3, number);
    if (includeIdentity) {
        appendField(manifest, 4, std::vector<std::uint8_t>(16, 0));
    }

    constexpr std::string_view keyText = "manifest";
    MDB_val key{keyText.size(), const_cast<char*>(keyText.data())};
    MDB_val value{manifest.size(), manifest.data()};
    REQUIRE(mdb_put(transaction, metadata, &key, &value, 0) == MDB_SUCCESS);
    REQUIRE(mdb_txn_commit(transaction) == MDB_SUCCESS);
    mdb_env_close(environment);
}

auto
readBlockRecord(const std::filesystem::path& path,
                hieda::notebook::BlockId blockId) -> std::vector<std::uint8_t>
{
    MDB_env* environment = nullptr;
    REQUIRE(mdb_env_create(&environment) == MDB_SUCCESS);
    REQUIRE(mdb_env_set_maxdbs(environment, 16) == MDB_SUCCESS);
    const auto encodedPath = lmdbFixturePath(path);
    REQUIRE(mdb_env_open(environment, encodedPath.c_str(),
                         MDB_NOSUBDIR | MDB_RDONLY, 0600) == MDB_SUCCESS);
    MDB_txn* transaction = nullptr;
    REQUIRE(mdb_txn_begin(environment, nullptr, MDB_RDONLY, &transaction) ==
            MDB_SUCCESS);
    MDB_dbi blocks = 0;
    REQUIRE(mdb_dbi_open(transaction, "blocks", 0, &blocks) == MDB_SUCCESS);
    MDB_val key{blockId.bytes.size(), blockId.bytes.data()};
    MDB_val value{};
    REQUIRE(mdb_get(transaction, blocks, &key, &value) == MDB_SUCCESS);
    const auto* bytes = static_cast<const std::uint8_t*>(value.mv_data);
    std::vector<std::uint8_t> record(bytes, bytes + value.mv_size);
    mdb_txn_abort(transaction);
    mdb_env_close(environment);
    return record;
}

void
writeBlockRecord(const std::filesystem::path& path,
                 hieda::notebook::BlockId blockId,
                 std::vector<std::uint8_t> record)
{
    MDB_env* environment = nullptr;
    REQUIRE(mdb_env_create(&environment) == MDB_SUCCESS);
    REQUIRE(mdb_env_set_maxdbs(environment, 16) == MDB_SUCCESS);
    const auto encodedPath = lmdbFixturePath(path);
    REQUIRE(mdb_env_open(environment, encodedPath.c_str(), MDB_NOSUBDIR,
                         0600) == MDB_SUCCESS);
    MDB_txn* transaction = nullptr;
    REQUIRE(mdb_txn_begin(environment, nullptr, 0, &transaction) ==
            MDB_SUCCESS);
    MDB_dbi blocks = 0;
    REQUIRE(mdb_dbi_open(transaction, "blocks", 0, &blocks) == MDB_SUCCESS);
    MDB_val key{blockId.bytes.size(), blockId.bytes.data()};
    MDB_val value{record.size(), record.data()};
    REQUIRE(mdb_put(transaction, blocks, &key, &value, 0) == MDB_SUCCESS);
    REQUIRE(mdb_txn_commit(transaction) == MDB_SUCCESS);
    mdb_env_close(environment);
}

void
removePageLinkIndexes(const std::filesystem::path& path)
{
    MDB_env* environment = nullptr;
    REQUIRE(mdb_env_create(&environment) == MDB_SUCCESS);
    REQUIRE(mdb_env_set_maxdbs(environment, 16) == MDB_SUCCESS);
    const auto encodedPath = lmdbFixturePath(path);
    REQUIRE(mdb_env_open(environment, encodedPath.c_str(), MDB_NOSUBDIR,
                         0600) == MDB_SUCCESS);
    MDB_txn* transaction = nullptr;
    REQUIRE(mdb_txn_begin(environment, nullptr, 0, &transaction) ==
            MDB_SUCCESS);
    for (const auto* name : {"references_by_source", "references_by_target"}) {
        MDB_dbi database = 0;
        REQUIRE(mdb_dbi_open(transaction, name, 0, &database) == MDB_SUCCESS);
        REQUIRE(mdb_drop(transaction, database, 1) == MDB_SUCCESS);
    }
    REQUIRE(mdb_txn_commit(transaction) == MDB_SUCCESS);
    mdb_env_close(environment);
}

auto
blockRecordTags(const std::vector<std::uint8_t>& record)
    -> std::vector<std::uint16_t>
{
    std::vector<std::uint16_t> tags;
    auto offset = std::size_t{2};
    while (offset < record.size()) {
        REQUIRE(record.size() - offset >= 6);
        const auto tag = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(record[offset]) << 8U) |
            record[offset + 1]);
        const auto length =
            (static_cast<std::uint32_t>(record[offset + 2]) << 24U) |
            (static_cast<std::uint32_t>(record[offset + 3]) << 16U) |
            (static_cast<std::uint32_t>(record[offset + 4]) << 8U) |
            record[offset + 5];
        tags.push_back(tag);
        offset += 6 + length;
        REQUIRE(offset <= record.size());
    }
    return tags;
}

} // namespace

TEST_CASE("a user can create a Notebook at a selected path")
{
    TemporaryDirectory temporaryDirectory;
    const auto notebookPath = temporaryDirectory.path() / "ideas.hieda";
    hieda::notebook::NotebookSession session;

    const auto result = session.create(notebookPath);

    REQUIRE(result);
    CHECK(session.isOpen());
    CHECK(result.value().path == notebookPath);
    CHECK(result.value().schemaVersion == 2);
    CHECK(std::filesystem::is_regular_file(notebookPath));
}

TEST_CASE("a created Notebook closes and reopens with the same identity")
{
    TemporaryDirectory temporaryDirectory;
    const auto notebookPath = temporaryDirectory.path() / "durable.hieda";
    hieda::notebook::NotebookSession session;
    const auto created = session.create(notebookPath);
    REQUIRE(created);
    const auto& createdInfo = created.value();

    session.close();
    CHECK_FALSE(session.isOpen());
    CHECK_FALSE(session.current().has_value());

    const auto reopened = session.open(notebookPath);
    REQUIRE(reopened);
    CHECK(reopened.value() == createdInfo);

    std::size_t canonicalFiles = 0;
    for (const auto& entry :
         std::filesystem::directory_iterator(temporaryDirectory.path())) {
        if (entry.path().extension() == ".hieda") {
            ++canonicalFiles;
        }
    }
    CHECK(canonicalFiles == 1);
}

TEST_CASE("a Notebook path can contain non-ASCII characters")
{
    TemporaryDirectory temporaryDirectory;
    const auto notebookPath =
        temporaryDirectory.path() / std::filesystem::path(u8"筆記.hieda");
    hieda::notebook::NotebookSession session;

    const auto created = session.create(notebookPath);
    REQUIRE(created);
    CHECK(created.value().path == notebookPath);

    session.close();
    const auto reopened = session.open(notebookPath);
    REQUIRE(reopened);
    CHECK(reopened.value().path == notebookPath);
}

TEST_CASE("creating never overwrites an existing path")
{
    TemporaryDirectory temporaryDirectory;
    const auto notebookPath = temporaryDirectory.path() / "existing.hieda";
    {
        std::ofstream existing(notebookPath);
        existing << "keep me";
    }
    hieda::notebook::NotebookSession session;

    const auto result = session.create(notebookPath);

    REQUIRE_FALSE(result);
    CHECK(result.error().code ==
          hieda::notebook::NotebookErrorCode::pathExists);
    std::ifstream existing(notebookPath);
    std::string contents;
    std::getline(existing, contents);
    CHECK(contents == "keep me");
}

TEST_CASE("opening invalid input returns a typed error")
{
    TemporaryDirectory temporaryDirectory;
    hieda::notebook::NotebookSession session;

    SECTION("missing path")
    {
        const auto result =
            session.open(temporaryDirectory.path() / "missing.hieda");
        REQUIRE_FALSE(result);
        CHECK(result.error().code ==
              hieda::notebook::NotebookErrorCode::pathNotFound);
    }

    SECTION("directory")
    {
        const auto result = session.open(temporaryDirectory.path());
        REQUIRE_FALSE(result);
        CHECK(result.error().code ==
              hieda::notebook::NotebookErrorCode::invalidPath);
    }

    SECTION("arbitrary file")
    {
        const auto path = temporaryDirectory.path() / "not-a-notebook.hieda";
        {
            std::ofstream file(path);
            file << "not a Notebook";
        }
        const auto result = session.open(path);
        REQUIRE_FALSE(result);
        CHECK(result.error().code ==
              hieda::notebook::NotebookErrorCode::invalidNotebook);
    }
}

TEST_CASE(
    "opening a newer Notebook schema returns an unsupported-version error")
{
    TemporaryDirectory temporaryDirectory;
    const auto notebookPath = temporaryDirectory.path() / "newer.hieda";
    createNotebookFixture(notebookPath, 3);
    hieda::notebook::NotebookSession session;

    const auto result = session.open(notebookPath);

    REQUIRE_FALSE(result);
    CHECK(result.error().code ==
          hieda::notebook::NotebookErrorCode::unsupportedVersion);
}

TEST_CASE("opening an incomplete Notebook manifest returns an invalid error")
{
    TemporaryDirectory temporaryDirectory;
    const auto notebookPath = temporaryDirectory.path() / "incomplete.hieda";
    createNotebookFixture(notebookPath, 2, false);
    hieda::notebook::NotebookSession session;

    const auto result = session.open(notebookPath);

    REQUIRE_FALSE(result);
    CHECK(result.error().code ==
          hieda::notebook::NotebookErrorCode::invalidNotebook);
}

TEST_CASE(
    "one session keeps its current Notebook when another open is attempted")
{
    TemporaryDirectory temporaryDirectory;
    const auto firstPath = temporaryDirectory.path() / "first.hieda";
    const auto secondPath = temporaryDirectory.path() / "second.hieda";
    hieda::notebook::NotebookSession session;
    hieda::notebook::NotebookSession setup;
    REQUIRE(setup.create(secondPath));
    setup.close();
    const auto first = session.create(firstPath);
    REQUIRE(first);

    const auto second = session.open(secondPath);

    REQUIRE_FALSE(second);
    CHECK(second.error().code ==
          hieda::notebook::NotebookErrorCode::alreadyOpen);
    const auto current = session.current();
    REQUIRE(current);
    const auto currentPath = current ? current->path : std::filesystem::path{};
    CHECK(currentPath == first.value().path);
}

TEST_CASE("a Notebook cannot be owned by two sessions")
{
    TemporaryDirectory temporaryDirectory;
    const auto notebookPath = temporaryDirectory.path() / "owned.hieda";
    hieda::notebook::NotebookSession firstSession;
    hieda::notebook::NotebookSession secondSession;
    REQUIRE(firstSession.create(notebookPath));

    const auto result = secondSession.open(notebookPath);

    REQUIRE_FALSE(result);
    CHECK(result.error().code ==
          hieda::notebook::NotebookErrorCode::alreadyInUse);
    firstSession.close();
    REQUIRE(secondSession.open(notebookPath));
}

TEST_CASE("file aliases cannot bypass Notebook ownership")
{
    TemporaryDirectory temporaryDirectory;
    const auto notebookPath = temporaryDirectory.path() / "canonical.hieda";
    const auto aliasPath = temporaryDirectory.path() / "alias.hieda";
    hieda::notebook::NotebookSession setup;
    REQUIRE(setup.create(notebookPath));
    setup.close();
    std::filesystem::create_hard_link(notebookPath, aliasPath);

    hieda::notebook::NotebookSession firstSession;
    hieda::notebook::NotebookSession secondSession;
    REQUIRE(firstSession.open(notebookPath));
    const auto result = secondSession.open(aliasPath);

    REQUIRE_FALSE(result);
    CHECK(result.error().code ==
          hieda::notebook::NotebookErrorCode::alreadyInUse);
}

TEST_CASE("destroying a session releases Notebook ownership")
{
    TemporaryDirectory temporaryDirectory;
    const auto notebookPath = temporaryDirectory.path() / "lifetime.hieda";
    {
        hieda::notebook::NotebookSession firstSession;
        REQUIRE(firstSession.create(notebookPath));
    }

    hieda::notebook::NotebookSession secondSession;
    REQUIRE(secondSession.open(notebookPath));
}

TEST_CASE("a failed open releases Notebook ownership")
{
    TemporaryDirectory temporaryDirectory;
    const auto invalidPath = temporaryDirectory.path() / "invalid-again.hieda";
    {
        std::ofstream file(invalidPath);
        file << "not a Notebook";
    }
    hieda::notebook::NotebookSession firstSession;
    REQUIRE_FALSE(firstSession.open(invalidPath));

    hieda::notebook::NotebookSession secondSession;
    const auto result = secondSession.open(invalidPath);
    REQUIRE_FALSE(result);
    CHECK(result.error().code ==
          hieda::notebook::NotebookErrorCode::invalidNotebook);
}

TEST_CASE("creating requires an existing parent directory")
{
    TemporaryDirectory temporaryDirectory;
    hieda::notebook::NotebookSession session;

    const auto result =
        session.create(temporaryDirectory.path() / "missing" / "notes.hieda");

    REQUIRE_FALSE(result);
    CHECK(result.error().code ==
          hieda::notebook::NotebookErrorCode::invalidPath);
}

TEST_CASE("creating reports a permission-denied parent directory")
{
#ifdef _WIN32
    SKIP("Windows ACL coverage requires a platform integration test");
#else
    if (geteuid() == 0) {
        SKIP("root can bypass directory write permissions");
    }
    TemporaryDirectory temporaryDirectory;
    const auto restrictedDirectory = temporaryDirectory.path() / "restricted";
    std::filesystem::create_directory(restrictedDirectory);
    std::filesystem::permissions(restrictedDirectory,
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::replace);
    hieda::notebook::NotebookSession session;

    const auto result = session.create(restrictedDirectory / "notes.hieda");
    std::filesystem::permissions(restrictedDirectory,
                                 std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace);

    REQUIRE_FALSE(result);
    CHECK(result.error().code ==
          hieda::notebook::NotebookErrorCode::permissionDenied);
#endif
}

TEST_CASE("a Journal Page stays virtual until its first Entry is committed")
{
    TemporaryDirectory temporaryDirectory;
    const auto notebookPath = temporaryDirectory.path() / "lazy-journal.hieda";
    const hieda::notebook::JournalDate date{2026, 8, 7};
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(notebookPath));

    const auto empty = session.outline(date);
    REQUIRE(empty);
    CHECK(empty.value().journalDate == date);
    CHECK_FALSE(empty.value().metadata.has_value());
    CHECK(empty.value().entries.empty());

    session.close();
    REQUIRE(session.open(notebookPath));
    const auto reopened = session.outline(date);
    REQUIRE(reopened);
    CHECK_FALSE(reopened.value().metadata.has_value());
}

TEST_CASE("titled Pages preserve unique names identity and contents across "
          "rename and reopen")
{
    TemporaryDirectory temporaryDirectory;
    const auto path = temporaryDirectory.path() / "pages.hieda";
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(path));

    const auto created = session.createPage("project_alpha", "Project Alpha");
    REQUIRE(created);
    const auto pageId = created.value().metadata.id;
    REQUIRE(session.createPage("second", "Project Alpha"));

    const auto duplicate = session.createPage("project_alpha", "Another title");
    REQUIRE_FALSE(duplicate);
    CHECK(duplicate.error().code ==
          hieda::notebook::NotebookErrorCode::pageNameConflict);
    CHECK_FALSE(session.createPage("Project", "Invalid name"));
    CHECK_FALSE(session.createPage("empty-title", ""));

    auto page = session.insertEntry(pageId, std::nullopt, "parent").value();
    page =
        session.insertEntry(pageId, page.entries.front().metadata.id, "child")
            .value();
    const auto parentId = page.entries.front().metadata.id;
    const auto childId = page.entries.back().metadata.id;
    page =
        session.moveEntry(childId, hieda::notebook::EntryMove::indent, "child")
            .value();
    CHECK(page.entries.back().parentEntry == parentId);
    CHECK(session.editCapabilities().value().canUndo);
    const auto undone = session.undoEdit();
    REQUIRE(undone);
    CHECK_FALSE(undone.value().front().entries.back().parentEntry);
    const auto redone = session.redoEdit();
    REQUIRE(redone);
    CHECK(redone.value().front().entries.back().parentEntry == parentId);

    const auto renamed =
        session.renamePage(pageId, "renamed_project", "Renamed Project");
    REQUIRE(renamed);
    CHECK(renamed.value().metadata.id == pageId);
    CHECK(renamed.value().entries.front().metadata.id == parentId);
    CHECK(renamed.value().entries.back().metadata.id == childId);

    session.close();
    REQUIRE(session.open(path));
    const auto reopened = session.outline(pageId);
    REQUIRE(reopened);
    CHECK(reopened.value().metadata == renamed.value().metadata);
    CHECK(reopened.value().name == renamed.value().name);
    CHECK(reopened.value().displayTitle == renamed.value().displayTitle);
    CHECK(reopened.value().entries == renamed.value().entries);
    REQUIRE(session.pages());
    CHECK(session.pages().value().size() == 2);
    CHECK(session.pages().value().front().name == "renamed_project");
}

TEST_CASE("Page Hierarchy derives previews and pages from hierarchical names")
{
    TemporaryDirectory temporaryDirectory;
    const auto path = temporaryDirectory.path() / "page-hierarchy.hieda";
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(path));
    auto notifications = 0;
    const auto subscription = session.subscribeToChanges(
        [&notifications]() -> void { ++notifications; });

    const auto descendant = session.createPage("work/client/alpha", "Alpha");
    REQUIRE(descendant);
    CHECK(notifications == 1);
    CHECK(
        session.current().value_or(hieda::notebook::NotebookInfo{}).revision ==
        1);
    REQUIRE(session.createPage("work/zeta", "Zeta"));
    const auto archive = session.createPage("archive", "Archive");
    REQUIRE(archive);
    CHECK_FALSE(session.createPage("work//broken", "Broken"));
    CHECK_FALSE(session.createPage("work/Uppercase", "Broken"));
    CHECK_FALSE(session.createPage(std::string(256, 'a'), "Broken"));
    CHECK(notifications == 3);
    CHECK(
        session.current().value_or(hieda::notebook::NotebookInfo{}).revision ==
        3);
    hieda::notebook::NotebookSessionTestAccess::rejectNextCommit(session);
    const auto failedCreate = session.createPage("failed/hierarchy", "Failed");
    REQUIRE_FALSE(failedCreate);
    CHECK(
        session.current().value_or(hieda::notebook::NotebookInfo{}).revision ==
        3);
    CHECK(notifications == 3);
    CHECK_FALSE(session.pageHierarchyNode("failed").value());

    const auto page = session.pageHierarchyNode("work/client/alpha");
    REQUIRE(page);
    REQUIRE(page.value());
    REQUIRE(page.value()->page);
    CHECK(page.value()->page->metadata.id == descendant.value().metadata.id);
    CHECK(page.value()->localSegment == "alpha");
    CHECK_FALSE(page.value()->hasChildren);

    const auto preview = session.pageHierarchyNode("work/client");
    REQUIRE(preview);
    REQUIRE(preview.value());
    CHECK_FALSE(preview.value()->page);
    CHECK(preview.value()->localSegment == "client");
    CHECK(preview.value()->hasChildren);
    REQUIRE(session.pageHierarchyNode("work"));
    REQUIRE(session.pageHierarchyNode("work").value());
    CHECK_FALSE(session.pageHierarchyNode("missing").value());

    const auto roots = session.pageHierarchyChildren();
    REQUIRE(roots);
    REQUIRE(roots.value().nodes.size() == 2);
    CHECK(roots.value().nodes[0].name == "archive");
    CHECK(roots.value().nodes[1].name == "work");
    CHECK_FALSE(roots.value().continuationCursor);
    const auto workChildren = session.pageHierarchyChildren("work");
    REQUIRE(workChildren);
    REQUIRE(workChildren.value().nodes.size() == 2);
    CHECK(workChildren.value().nodes[0].name == "work/client");
    CHECK(workChildren.value().nodes[1].name == "work/zeta");

    REQUIRE(session.deletePage(archive.value().metadata.id));
    CHECK_FALSE(session.pageHierarchyNode("archive").value());
    REQUIRE(session.undoEdit());
    REQUIRE(session.pageHierarchyNode("archive").value()->page);
    REQUIRE(session.redoEdit());
    CHECK_FALSE(session.pageHierarchyNode("archive").value());

    for (int index = 0; index < 101; ++index) {
        REQUIRE(session.createPage("many/p" + std::to_string(1000 + index),
                                   "Many"));
    }
    const auto firstBatch = session.pageHierarchyChildren("many");
    REQUIRE(firstBatch);
    CHECK(firstBatch.value().nodes.size() == 100);
    REQUIRE(firstBatch.value().continuationCursor);
    const auto secondBatch = session.pageHierarchyChildren(
        "many", firstBatch.value().continuationCursor);
    REQUIRE(secondBatch);
    CHECK(secondBatch.value().nodes.size() == 1);
    REQUIRE(session.createPage("many/p9999", "Later"));
    const auto stale = session.pageHierarchyChildren(
        "many", firstBatch.value().continuationCursor);
    REQUIRE_FALSE(stale);
    CHECK(stale.error().code ==
          hieda::notebook::NotebookErrorCode::staleHierarchyCursor);

    const auto materialized = session.createPage("work/client", "Client");
    REQUIRE(materialized);
    REQUIRE(session.pageHierarchyNode("work/client").value()->page);
    const auto materializedRevision =
        session.current().value_or(hieda::notebook::NotebookInfo{}).revision;
    hieda::notebook::NotebookSessionTestAccess::rejectNextCommit(session);
    const auto failedUndo = session.undoEdit();
    REQUIRE_FALSE(failedUndo);
    CHECK(
        session.current().value_or(hieda::notebook::NotebookInfo{}).revision ==
        materializedRevision);
    REQUIRE(session.pageHierarchyNode("work/client").value()->page);
    CHECK(session.editCapabilities().value().canUndo);
    REQUIRE(session.undoEdit());
    CHECK(
        session.current().value_or(hieda::notebook::NotebookInfo{}).revision ==
        materializedRevision + 1);
    REQUIRE(session.pageHierarchyNode("work/client").value());
    CHECK_FALSE(session.pageHierarchyNode("work/client").value()->page);
    const auto undoneRevision =
        session.current().value_or(hieda::notebook::NotebookInfo{}).revision;
    hieda::notebook::NotebookSessionTestAccess::rejectNextCommit(session);
    const auto failedRedo = session.redoEdit();
    REQUIRE_FALSE(failedRedo);
    CHECK(
        session.current().value_or(hieda::notebook::NotebookInfo{}).revision ==
        undoneRevision);
    CHECK_FALSE(session.pageHierarchyNode("work/client").value()->page);
    CHECK(session.editCapabilities().value().canRedo);
    REQUIRE(session.redoEdit());
    REQUIRE(session.pageHierarchyNode("work/client").value()->page);
    CHECK(session.pageHierarchyNode("work/client").value()->page->metadata ==
          materialized.value().metadata);

    const auto withEntry = session.insertEntry(materialized.value().metadata.id,
                                               std::nullopt, "durable");
    REQUIRE(withEntry);

    const auto deleted = session.deletePage(materialized.value().metadata.id);
    REQUIRE(deleted);
    REQUIRE(deleted.value().entries.size() == 1);
    CHECK(deleted.value().entries.front().authoredText == "durable");
    REQUIRE(session.pageHierarchyNode("work/client").value());
    CHECK_FALSE(session.pageHierarchyNode("work/client").value()->page);
    REQUIRE(session.undoEdit());
    REQUIRE(session.pageHierarchyNode("work/client").value()->page);
    CHECK(session.pageHierarchyNode("work/client").value()->page->metadata ==
          deleted.value().metadata);
    CHECK(session.outline(materialized.value().metadata.id).value().entries ==
          deleted.value().entries);
    REQUIRE(session.redoEdit());
    CHECK_FALSE(session.pageHierarchyNode("work/client").value()->page);

    const auto revision =
        session.current().value_or(hieda::notebook::NotebookInfo{}).revision;
    const auto notificationCount = notifications;
    hieda::notebook::NotebookSessionTestAccess::rejectNextCommit(session);
    const auto failedDelete =
        session.deletePage(descendant.value().metadata.id);
    REQUIRE_FALSE(failedDelete);
    CHECK(
        session.current().value_or(hieda::notebook::NotebookInfo{}).revision ==
        revision);
    CHECK(notifications == notificationCount);
    REQUIRE(session.pageHierarchyNode("work/client/alpha").value()->page);

    session.close();
    REQUIRE(session.open(path));
    REQUIRE(session.pageHierarchyNode("work/client/alpha").value()->page);
    REQUIRE(session.pageHierarchyNode("work/client").value());
    CHECK_FALSE(session.pageHierarchyNode("work/client").value()->page);
}

TEST_CASE("committed Page Link notation resolves exact names and keeps invalid "
          "text inert")
{
    TemporaryDirectory temporaryDirectory;
    const auto path = temporaryDirectory.path() / "page-links.hieda";
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(path));
    const auto target =
        session.createPage("work/client_alpha", "Client Alpha").value();
    const auto source = session.createPage("source", "Source").value();
    const auto text = std::string{
        "See [[work/client_alpha]] and [[missing/page]].\n"
        "Escaped \\[[work/client_alpha]] and malformed [[Uppercase]].\n"
        "opaque::[[work/client_alpha]]"};
    const auto entry =
        session.insertEntry(source.metadata.id, std::nullopt, text)
            .value()
            .entries[0];

    const auto links = session.pageLinks(entry.metadata.id);

    REQUIRE(links);
    REQUIRE(links.value().size() == 2);
    CHECK(links.value()[0].pageName == "work/client_alpha");
    REQUIRE(links.value()[0].target);
    CHECK(links.value()[0]
              .target.value_or(hieda::notebook::PageSummary{})
              .metadata.id == target.metadata.id);
    CHECK(links.value()[0]
              .target.value_or(hieda::notebook::PageSummary{})
              .displayTitle == "Client Alpha");
    CHECK(text.substr(links.value()[0].sourceByteOffset,
                      links.value()[0].sourceByteLength) ==
          "[[work/client_alpha]]");
    CHECK(links.value()[1].pageName == "missing/page");
    CHECK_FALSE(links.value()[1].target);
}

TEST_CASE("Page Link parsing applies line-local pairing property opacity and "
          "escape parity")
{
    TemporaryDirectory temporaryDirectory;
    const auto path = temporaryDirectory.path() / "page-link-grammar.hieda";
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(path));
    REQUIRE(session.createPage("target", "Target"));
    const auto sourcePage = session.createPage("source", "Source").value();
    const auto text =
        std::string{R"([[target]] \[[target]] \\[[target]] [[bad[[target]]]]
work/client::[[target]]
\work/client::[[target]])"};
    const auto entry =
        session.insertEntry(sourcePage.metadata.id, std::nullopt, text)
            .value()
            .entries[0];

    const auto links = session.pageLinks(entry.metadata.id);

    REQUIRE(links);
    REQUIRE(links.value().size() == 3);
    CHECK(std::ranges::all_of(links.value(), [](const auto& link) -> bool {
        return link.pageName == "target" && link.target.has_value();
    }));
    CHECK(text.substr(links.value()[0].sourceByteOffset,
                      links.value()[0].sourceByteLength) == "[[target]]");
    CHECK(text.substr(links.value()[1].sourceByteOffset,
                      links.value()[1].sourceByteLength) == "[[target]]");
    CHECK(text.substr(links.value()[2].sourceByteOffset,
                      links.value()[2].sourceByteLength) == "[[target]]");
}

TEST_CASE("Authored Text accepts one MiB and rejects forbidden controls or "
          "larger input")
{
    TemporaryDirectory temporaryDirectory;
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(temporaryDirectory.path() /
                           "authored-text-limits.hieda"));
    const auto page = session.createPage("limits", "Limits").value();

    REQUIRE(session.insertEntry(page.metadata.id, std::nullopt,
                                std::string(1024ULL * 1024ULL, 'a')));
    CHECK_FALSE(session.insertEntry(page.metadata.id, std::nullopt,
                                    std::string((1024ULL * 1024ULL) + 1, 'a')));
    CHECK_FALSE(
        session.insertEntry(page.metadata.id, std::nullopt, "tab\ttext"));
    CHECK_FALSE(
        session.insertEntry(page.metadata.id, std::nullopt,
                            std::string{"c1"} + std::string{"\xC2\x80", 2}));
}

TEST_CASE("following Page Links opens materialized Pages or exact "
          "non-materialized previews")
{
    TemporaryDirectory temporaryDirectory;
    const auto path = temporaryDirectory.path() / "follow-page-links.hieda";
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(path));
    const auto target = session.createPage("target", "Target title").value();
    const auto sourcePage = session.createPage("source", "Source").value();
    const auto first = session
                           .insertEntry(sourcePage.metadata.id, std::nullopt,
                                        "[[target]] then [[missing/exact]]")
                           .value()
                           .entries[0];
    const auto second =
        session
            .insertEntry(sourcePage.metadata.id, first.metadata.id,
                         "again [[missing/exact]] not [[missing/other]]")
            .value()
            .entries[1];
    const auto links = session.pageLinks(first.metadata.id).value();

    const auto resolved = session.followPageLink(first.metadata.id,
                                                 links[0].sourceByteOffset + 3);
    REQUIRE(resolved);
    REQUIRE(
        std::holds_alternative<hieda::notebook::PageSummary>(resolved.value()));
    CHECK(
        std::get<hieda::notebook::PageSummary>(resolved.value()).metadata.id ==
        target.metadata.id);

    const auto unresolved =
        session.followPageLink(first.metadata.id, links[1].sourceByteOffset);
    REQUIRE(unresolved);
    REQUIRE(std::holds_alternative<hieda::notebook::PagePreview>(
        unresolved.value()));
    const auto& preview =
        std::get<hieda::notebook::PagePreview>(unresolved.value());
    CHECK(preview.name == "missing/exact");
    REQUIRE(preview.sources.size() == 2);
    CHECK(std::ranges::any_of(preview.sources, [&](const auto& source) -> bool {
        return source.metadata.id == first.metadata.id;
    }));
    CHECK(std::ranges::any_of(preview.sources, [&](const auto& source) -> bool {
        return source.metadata.id == second.metadata.id;
    }));
    CHECK_FALSE(session.pageHierarchyNode("missing/exact").value());

    const auto directPreview = session.pagePreview("missing/exact");
    REQUIRE(directPreview);
    CHECK(directPreview.value() == preview);
}

TEST_CASE("Page rename atomically rewrites resolved Page Links without "
          "touching source times")
{
    TemporaryDirectory temporaryDirectory;
    const auto path = temporaryDirectory.path() / "rename-page-links.hieda";
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(path));
    const auto target = session.createPage("work/alpha", "Alpha").value();
    const auto sourcePage = session.createPage("source", "Source").value();
    const auto originalText = std::string{
        "[[work/alpha]] twice [[work/alpha]] and waiting [[work/beta]]; "
        "escaped \\[[work/alpha]]"};
    const auto source =
        session.insertEntry(sourcePage.metadata.id, std::nullopt, originalText)
            .value()
            .entries[0];
    const auto revision =
        session.current().value_or(hieda::notebook::NotebookInfo{}).revision;

    hieda::notebook::NotebookSessionTestAccess::rejectNextCommit(session);
    const auto failedRename =
        session.renamePage(target.metadata.id, "work/gamma", "Gamma");
    REQUIRE_FALSE(failedRename);
    CHECK(
        session.current().value_or(hieda::notebook::NotebookInfo{}).revision ==
        revision);
    CHECK(session.outline(target.metadata.id).value().name == "work/alpha");
    CHECK(session.outline(sourcePage.metadata.id)
              .value()
              .entries[0]
              .authoredText == originalText);
    CHECK(session.outline(sourcePage.metadata.id)
              .value()
              .entries[0]
              .metadata.updatedAt == source.metadata.updatedAt);

    const auto renamed =
        session.renamePage(target.metadata.id, "work/beta", "Beta");

    REQUIRE(renamed);
    CHECK(
        session.current().value_or(hieda::notebook::NotebookInfo{}).revision ==
        revision + 1);
    const auto rewritten =
        session.outline(sourcePage.metadata.id).value().entries[0];
    CHECK(
        rewritten.authoredText ==
        "[[work/beta]] twice [[work/beta]] and waiting [[work/beta]]; escaped "
        "\\[[work/alpha]]");
    CHECK(rewritten.metadata.updatedAt == source.metadata.updatedAt);
    const auto rewrittenLinks = session.pageLinks(source.metadata.id).value();
    REQUIRE(rewrittenLinks.size() == 3);
    CHECK(std::ranges::all_of(rewrittenLinks, [&](const auto& link) -> bool {
        return link.target &&
               link.target.value().metadata.id == target.metadata.id;
    }));

    const auto undone = session.undoEdit();
    REQUIRE(undone);
    CHECK(session.outline(target.metadata.id).value().name == "work/alpha");
    const auto restored =
        session.outline(sourcePage.metadata.id).value().entries[0];
    CHECK(restored.authoredText == originalText);
    CHECK(restored.metadata.updatedAt == source.metadata.updatedAt);

    REQUIRE(session.redoEdit());
    CHECK(session.outline(sourcePage.metadata.id)
              .value()
              .entries[0]
              .authoredText == rewritten.authoredText);
    session.close();
    REQUIRE(session.open(path));
    CHECK(session.outline(sourcePage.metadata.id)
              .value()
              .entries[0]
              .authoredText == rewritten.authoredText);
}

TEST_CASE(
    "Page deletion and recreation transition incoming links by conceptual name")
{
    TemporaryDirectory temporaryDirectory;
    const auto path = temporaryDirectory.path() / "delete-page-links.hieda";
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(path));
    const auto target = session.createPage("concept", "Concept").value();
    const auto sourcePage = session.createPage("source", "Source").value();
    const auto source =
        session.insertEntry(sourcePage.metadata.id, std::nullopt, "[[concept]]")
            .value()
            .entries[0];

    REQUIRE(session.deletePage(target.metadata.id));
    const auto unresolved = session.pageLinks(source.metadata.id).value()[0];
    CHECK_FALSE(unresolved.target);
    CHECK(session.outline(sourcePage.metadata.id)
              .value()
              .entries[0]
              .metadata.updatedAt == source.metadata.updatedAt);
    CHECK(session.pagePreview("concept").value().sources.size() == 1);

    REQUIRE(session.undoEdit());
    const auto restored = session.pageLinks(source.metadata.id).value()[0];
    REQUIRE(restored.target);
    CHECK(
        restored.target.value_or(hieda::notebook::PageSummary{}).metadata.id ==
        target.metadata.id);
    CHECK(session.outline(sourcePage.metadata.id)
              .value()
              .entries[0]
              .metadata.updatedAt == source.metadata.updatedAt);

    REQUIRE(session.redoEdit());
    const auto recreated = session.createPage("concept", "Recreated").value();
    CHECK(recreated.metadata.id != target.metadata.id);
    const auto reresolved = session.pageLinks(source.metadata.id).value()[0];
    REQUIRE(reresolved.target);
    CHECK(reresolved.target.value_or(hieda::notebook::PageSummary{})
              .metadata.id == recreated.metadata.id);
    CHECK(session.outline(sourcePage.metadata.id)
              .value()
              .entries[0]
              .metadata.updatedAt == source.metadata.updatedAt);
}

TEST_CASE("Entry commits replace Page Link meaning atomically and persist it "
          "across reopen")
{
    TemporaryDirectory temporaryDirectory;
    const auto path = temporaryDirectory.path() / "edit-page-links.hieda";
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(path));
    const auto firstTarget = session.createPage("first", "First").value();
    const auto secondTarget = session.createPage("second", "Second").value();
    const auto sourcePage = session.createPage("source", "Source").value();
    const auto entry =
        session.insertEntry(sourcePage.metadata.id, std::nullopt, "[[first]]")
            .value()
            .entries[0];
    REQUIRE(session.pageLinks(entry.metadata.id).value()[0].target);
    CHECK(session.pageLinks(entry.metadata.id)
              .value()[0]
              .target.value_or(hieda::notebook::PageSummary{})
              .metadata.id == firstTarget.metadata.id);

    hieda::notebook::NotebookSessionTestAccess::rejectNextCommit(session);
    const auto failed = session.updateEntry(entry.metadata.id, "[[second]]");
    REQUIRE_FALSE(failed);
    REQUIRE(session.pageLinks(entry.metadata.id).value()[0].target);
    CHECK(session.pageLinks(entry.metadata.id)
              .value()[0]
              .target.value_or(hieda::notebook::PageSummary{})
              .metadata.id == firstTarget.metadata.id);

    REQUIRE(session.updateEntry(entry.metadata.id, "[[second]] [[second]]"));
    const auto replaced = session.pageLinks(entry.metadata.id).value();
    REQUIRE(replaced.size() == 2);
    CHECK(std::ranges::all_of(replaced, [&](const auto& link) -> bool {
        return link.target &&
               link.target.value().metadata.id == secondTarget.metadata.id;
    }));
    REQUIRE(session.updateEntry(entry.metadata.id, "plain [[incomplete"));
    CHECK(session.pageLinks(entry.metadata.id).value().empty());

    session.close();
    REQUIRE(session.open(path));
    CHECK(session.pageLinks(entry.metadata.id).value().empty());
}

TEST_CASE("a user inserts follows and reopens a durable Block Reference")
{
    TemporaryDirectory temporaryDirectory;
    const auto notebookPath =
        temporaryDirectory.path() / "block-reference.hieda";
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(notebookPath));
    const auto targetPage = session.createPage("target", "Target").value();
    const auto target =
        session.insertEntry(targetPage.metadata.id, std::nullopt, "target text")
            .value()
            .entries[0];
    const auto child =
        session.insertEntry(targetPage.metadata.id, target.metadata.id, "child")
            .value()
            .entries[1];
    REQUIRE(session.moveEntry(child.metadata.id,
                              hieda::notebook::EntryMove::indent, "child"));
    const auto sourcePage = session.createPage("source", "Source").value();
    const auto source =
        session
            .insertEntry(sourcePage.metadata.id, std::nullopt, "before after")
            .value()
            .entries[0];

    const auto inserted =
        session.insertBlockReference(source.metadata.id, 7, child.metadata.id);

    REQUIRE(inserted);
    const auto notation = "[[block:" + child.metadata.id.toString() + "]]";
    CHECK(inserted.value().authoredText == "before " + notation + "after");
    const auto references = session.blockReferences(source.metadata.id);
    REQUIRE(references);
    REQUIRE(references.value().size() == 1);
    CHECK(references.value()[0].targetId == child.metadata.id);
    REQUIRE(references.value()[0].target);
    const auto followed = session.followBlockReference(
        source.metadata.id, references.value()[0].sourceByteOffset + 3);
    REQUIRE(followed);
    CHECK(followed.value().target.id == child.metadata.id);
    CHECK(followed.value().structuralPage.metadata.value().id ==
          targetPage.metadata.id);
    REQUIRE(followed.value().containmentPath.size() == 2);
    CHECK(followed.value().containmentPath[0] == target.metadata.id);
    CHECK(followed.value().containmentPath[1] == child.metadata.id);

    session.close();
    REQUIRE(session.open(notebookPath));
    REQUIRE(session.blockReferences(source.metadata.id).value()[0].target);
    CHECK(
        session.followBlockReference(source.metadata.id, 7).value().target.id ==
        child.metadata.id);
}

TEST_CASE(
    "Linked References deduplicate occurrences and track structural changes")
{
    TemporaryDirectory temporaryDirectory;
    hieda::notebook::NotebookSession session;
    REQUIRE(
        session.create(temporaryDirectory.path() / "linked-references.hieda"));
    const auto targetPage = session.createPage("target", "Target").value();
    const auto sourcePage = session.createPage("source", "Source").value();
    const auto archivePage = session.createPage("archive", "Archive").value();
    const auto sourceText =
        "[[target]] and [[block:" + targetPage.metadata.id.toString() +
        "]] twice [[target]]";
    const auto source =
        session.insertEntry(sourcePage.metadata.id, std::nullopt, sourceText)
            .value()
            .entries[0];

    auto incoming = session.linkedReferences(targetPage.metadata.id);

    REQUIRE(incoming);
    CHECK(incoming.value().totalSourceCount == 1);
    REQUIRE(incoming.value().sources.size() == 1);
    CHECK(incoming.value().sources[0].source.metadata.id == source.metadata.id);
    CHECK(incoming.value().sources[0].occurrenceCount == 3);
    REQUIRE(incoming.value().sources[0].occurrences.size() == 3);
    CHECK(incoming.value().sources[0].occurrences[0].kind ==
          hieda::notebook::SemanticReferenceKind::pageLink);
    CHECK(incoming.value().sources[0].occurrences[1].kind ==
          hieda::notebook::SemanticReferenceKind::blockReference);
    CHECK(incoming.value().sources[0].structuralPage.metadata.value().id ==
          sourcePage.metadata.id);

    REQUIRE(session.moveEntryToPage(source.metadata.id, archivePage.metadata.id,
                                    std::nullopt));
    incoming = session.linkedReferences(targetPage.metadata.id);
    REQUIRE(incoming);
    CHECK(incoming.value().sources[0].structuralPage.metadata.value().id ==
          archivePage.metadata.id);

    REQUIRE(session.updateEntry(source.metadata.id, "no references"));
    CHECK(session.linkedReferences(targetPage.metadata.id)
              .value()
              .totalSourceCount == 0);
}

TEST_CASE(
    "deleting and undoing a target only changes Block Reference resolution")
{
    TemporaryDirectory temporaryDirectory;
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(temporaryDirectory.path() /
                           "missing-block-reference.hieda"));
    const auto targetPage = session.createPage("target", "Target").value();
    const auto target =
        session.insertEntry(targetPage.metadata.id, std::nullopt, "target")
            .value()
            .entries[0];
    const auto archivePage = session.createPage("archive", "Archive").value();
    const auto sourcePage = session.createPage("source", "Source").value();
    const auto sourceText =
        "points to [[block:" + target.metadata.id.toString() + "]]";
    const auto source =
        session.insertEntry(sourcePage.metadata.id, std::nullopt, sourceText)
            .value()
            .entries[0];

    REQUIRE(session.moveEntryToPage(target.metadata.id, archivePage.metadata.id,
                                    std::nullopt));
    CHECK(
        session.linkedReferences(target.metadata.id).value().totalSourceCount ==
        1);
    CHECK(session.followBlockReference(source.metadata.id, 10)
              .value()
              .structuralPage.metadata.value()
              .id == archivePage.metadata.id);

    REQUIRE(session.deleteEntry(target.metadata.id));

    const auto missing = session.blockReferences(source.metadata.id).value()[0];
    CHECK_FALSE(missing.target);
    CHECK(session.outline(sourcePage.metadata.id)
              .value()
              .entries[0]
              .authoredText == sourceText);
    CHECK(session.outline(sourcePage.metadata.id)
              .value()
              .entries[0]
              .metadata.updatedAt == source.metadata.updatedAt);
    const auto missingFollow =
        session.followBlockReference(source.metadata.id, 10);
    CHECK_FALSE(missingFollow);
    CHECK(missingFollow.error().code ==
          hieda::notebook::NotebookErrorCode::blockNotFound);

    REQUIRE(session.undoEdit());
    REQUIRE(session.blockReferences(source.metadata.id).value()[0].target);
    CHECK(session.followBlockReference(source.metadata.id, 10)
              .value()
              .target.id == target.metadata.id);
    CHECK(session.outline(sourcePage.metadata.id)
              .value()
              .entries[0]
              .authoredText == sourceText);
    CHECK(session.outline(sourcePage.metadata.id)
              .value()
              .entries[0]
              .metadata.updatedAt == source.metadata.updatedAt);
}

TEST_CASE(
    "Linked References batches and cursors are bounded to a Notebook revision")
{
    TemporaryDirectory temporaryDirectory;
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(temporaryDirectory.path() /
                           "linked-reference-cursor.hieda"));
    const auto target = session.createPage("target", "Target").value();
    const auto source = session.createPage("source", "Source").value();
    const auto notation = "[[block:" + target.metadata.id.toString() + "]]";
    for (auto index = 0; index < 101; ++index) {
        REQUIRE(session.insertEntry(source.metadata.id, std::nullopt,
                                    notation + notation + notation + notation));
    }

    const auto first = session.linkedReferences(target.metadata.id);

    REQUIRE(first);
    CHECK(first.value().totalSourceCount == 101);
    CHECK(first.value().sources.size() == 100);
    REQUIRE(first.value().continuationCursor);
    CHECK(first.value().sources[0].occurrenceCount == 4);
    CHECK(first.value().sources[0].occurrences.size() == 3);
    const auto firstOccurrences = session.linkedReferenceOccurrences(
        target.metadata.id, first.value().sources[0].source.metadata.id);
    REQUIRE(firstOccurrences);
    CHECK(firstOccurrences.value().totalOccurrenceCount == 4);
    CHECK(firstOccurrences.value().occurrences.size() == 3);
    REQUIRE(firstOccurrences.value().continuationCursor);
    const auto remainingOccurrences = session.linkedReferenceOccurrences(
        target.metadata.id, first.value().sources[0].source.metadata.id,
        firstOccurrences.value().continuationCursor);
    REQUIRE(remainingOccurrences);
    CHECK(remainingOccurrences.value().occurrences.size() == 1);
    CHECK_FALSE(remainingOccurrences.value().continuationCursor);
    const auto second = session.linkedReferences(
        target.metadata.id, first.value().continuationCursor);
    REQUIRE(second);
    CHECK(second.value().sources.size() == 1);
    CHECK_FALSE(second.value().continuationCursor);

    REQUIRE(session.updateEntry(second.value().sources[0].source.metadata.id,
                                "changed"));
    const auto stale = session.linkedReferences(
        target.metadata.id, first.value().continuationCursor);
    CHECK_FALSE(stale);
    CHECK(stale.error().code ==
          hieda::notebook::NotebookErrorCode::staleLinkedReferencesCursor);
}

TEST_CASE(
    "Linked References group Pages by recency and keep current outline order")
{
    TemporaryDirectory temporaryDirectory;
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(temporaryDirectory.path() /
                           "linked-reference-order.hieda"));
    const auto target = session.createPage("target", "Target").value();
    const auto firstPage = session.createPage("first", "First").value();
    const auto secondPage = session.createPage("second", "Second").value();
    const auto notation = "[[block:" + target.metadata.id.toString() + "]]";
    const auto first =
        session.insertEntry(firstPage.metadata.id, std::nullopt, notation)
            .value()
            .entries[0];
    const auto second =
        session.insertEntry(secondPage.metadata.id, std::nullopt, notation)
            .value()
            .entries[0];
    const auto third =
        session.insertEntry(firstPage.metadata.id, first.metadata.id, notation)
            .value()
            .entries[1];

    const auto incoming = session.linkedReferences(target.metadata.id).value();

    REQUIRE(incoming.sources.size() == 3);
    CHECK(incoming.sources[0].source.metadata.id == first.metadata.id);
    CHECK(incoming.sources[1].source.metadata.id == third.metadata.id);
    CHECK(incoming.sources[2].source.metadata.id == second.metadata.id);
}

TEST_CASE("unresolved Page Link sources use bounded preview batches")
{
    TemporaryDirectory temporaryDirectory;
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(temporaryDirectory.path() /
                           "preview-linked-references.hieda"));
    const auto source = session.createPage("source", "Source").value();
    for (auto index = 0; index < 101; ++index) {
        REQUIRE(session.insertEntry(
            source.metadata.id, std::nullopt,
            "[[missing]] [[missing]] [[missing]] [[missing]]"));
    }

    const auto first = session.unresolvedPageLinkSources("missing");
    REQUIRE(first);
    CHECK(first.value().totalSourceCount == 101);
    CHECK(first.value().sources.size() == 100);
    REQUIRE(first.value().continuationCursor);
    CHECK(first.value().sources.front().occurrenceCount == 4);
    CHECK(first.value().sources.front().occurrences.size() == 3);
    const auto more = session.unresolvedPageLinkOccurrences(
        "missing", first.value().sources.front().source.metadata.id);
    REQUIRE(more);
    REQUIRE(more.value().continuationCursor);
    const auto last = session.unresolvedPageLinkOccurrences(
        "missing", first.value().sources.front().source.metadata.id,
        more.value().continuationCursor);
    REQUIRE(last);
    CHECK(last.value().occurrences.size() == 1);
}

TEST_CASE("opening a schema v2 Notebook backfills missing derived indexes")
{
    TemporaryDirectory temporaryDirectory;
    const auto notebookPath =
        temporaryDirectory.path() / "page-link-backfill.hieda";
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(notebookPath));
    REQUIRE(session.createPage("target", "Target"));
    const auto source = session.createPage("source", "Source").value();
    const auto entry = session
                           .insertEntry(source.metadata.id, std::nullopt,
                                        "[[target]]\nstatus::open")
                           .value()
                           .entries[0];
    const auto query =
        session
            .insertEntry(source.metadata.id, std::nullopt,
                         "{{query (where (property-equals status \"open\"))}}")
            .value()
            .entries.back();
    const auto revision =
        session.current().value_or(hieda::notebook::NotebookInfo{}).revision;

    session.close();
    removePageLinkIndexes(notebookPath);

    const auto reopened = session.open(notebookPath);
    REQUIRE(reopened);
    CHECK(reopened.value().revision == revision);
    const auto links = session.pageLinks(entry.metadata.id);
    REQUIRE(links);
    REQUIRE(links.value().size() == 1);
    CHECK(links.value()[0].pageName == "target");
    REQUIRE(links.value()[0].target);
    CHECK(links.value()[0]
              .target.value_or(hieda::notebook::PageSummary{})
              .displayTitle == "Target");
    const auto queryResults = session.evaluateQuery(query.metadata.id);
    REQUIRE(queryResults);
    REQUIRE(queryResults.value().rows.size() == 1);
    CHECK(queryResults.value().rows.front().metadata.id == entry.metadata.id);
}

TEST_CASE(
    "Page Entries provide complete shared outline commands and notifications")
{
    TemporaryDirectory temporaryDirectory;
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(temporaryDirectory.path() / "page-outline.hieda"));
    const auto firstPage = session.createPage("first", "First").value();
    const auto secondPage = session.createPage("second", "Second").value();
    auto notifications = 0;
    const auto subscription = session.subscribeToChanges(
        [&notifications]() -> void { ++notifications; });

    auto outline =
        session.insertEntry(firstPage.metadata.id, std::nullopt, "parent")
            .value();
    const auto parentId = outline.entries.front().metadata.id;
    outline =
        session.insertEntry(firstPage.metadata.id, parentId, "child").value();
    const auto childId = outline.entries.back().metadata.id;
    outline =
        session.insertEntry(firstPage.metadata.id, childId, "tail").value();
    const auto tailId = outline.entries.back().metadata.id;
    REQUIRE(session.updateEntry(childId, "child updated"));
    outline = session.splitEntry(childId, "child updated", 5).value();
    const auto splitId = outline.entries[2].metadata.id;
    outline = session.joinEntry(splitId, " updated").value();
    CHECK(outline.entries[1].authoredText == "child updated");
    outline = session
                  .moveEntry(childId, hieda::notebook::EntryMove::indent,
                             "child updated")
                  .value();
    CHECK(outline.entries[1].parentEntry == parentId);
    outline = session
                  .moveEntry(childId, hieda::notebook::EntryMove::outdent,
                             "child updated")
                  .value();
    CHECK_FALSE(outline.entries[1].parentEntry);
    outline = session
                  .moveEntry(childId, hieda::notebook::EntryMove::down,
                             "child updated")
                  .value();
    CHECK(outline.entries.back().metadata.id == childId);
    outline =
        session
            .moveEntry(childId, hieda::notebook::EntryMove::up, "child updated")
            .value();
    CHECK(outline.entries[1].metadata.id == childId);
    outline = session.deleteEntry(tailId).value();
    REQUIRE(outline.entries.size() == 2);
    outline = session.deleteSubtrees({parentId}).value();
    REQUIRE(outline.entries.size() == 1);
    CHECK(outline.entries.front().metadata.id == childId);
    CHECK(notifications == 12);

    const auto invalid = session.moveEntry(
        childId, hieda::notebook::EntryMove::up, "invalid\rtext");
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().code ==
          hieda::notebook::NotebookErrorCode::invalidAuthoredText);
    CHECK(notifications == 12);
    REQUIRE(session.undoEdit());
    REQUIRE(session.redoEdit());
    CHECK(notifications == 14);

    REQUIRE(session.insertEntry(secondPage.metadata.id, std::nullopt,
                                "independent"));
    CHECK(session.editCapabilities().value().canUndo);
    CHECK(session.editCapabilities().value().canUndo);
    REQUIRE(session.undoEdit());
    CHECK(session.outline(firstPage.metadata.id).value().entries.size() == 1);

    hieda::notebook::BlockId missingPage;
    missingPage.bytes.front() = std::byte{1};
    const auto missing = session.outline(missingPage);
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code ==
          hieda::notebook::NotebookErrorCode::pageNotFound);
    CHECK_FALSE(session.createPage("bad_title", "line\nbreak"));
    CHECK_FALSE(session.createPage("bad_utf8", std::string("\xC3", 1)));

    const auto firstBeforeCrossPage =
        session.outline(firstPage.metadata.id).value();
    const auto secondEntry =
        session.insertEntry(secondPage.metadata.id, std::nullopt, "other")
            .value()
            .entries.front()
            .metadata.id;
    const auto crossPage = session.deleteSubtrees({childId, secondEntry});
    REQUIRE_FALSE(crossPage);
    CHECK(crossPage.error().code ==
          hieda::notebook::NotebookErrorCode::blockNotFound);
    CHECK(session.outline(firstPage.metadata.id).value() ==
          firstBeforeCrossPage);
}

TEST_CASE("Page validation and failed commits preserve identity indexes "
          "revision and history")
{
    TemporaryDirectory temporaryDirectory;
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(temporaryDirectory.path() / "page-failures.hieda"));
    const auto maximumName = std::string("a") + std::string(63, '0');
    REQUIRE(session.createPage(maximumName, "Maximum"));
    CHECK_FALSE(session.createPage(std::string("a") + std::string(64, '0'),
                                   "Too long"));
    CHECK_FALSE(session.createPage("", "Empty"));
    CHECK_FALSE(session.createPage("Uppercase", "Uppercase"));
    CHECK_FALSE(session.createPage("1leading", "Leading digit"));
    CHECK_FALSE(session.createPage("has.dot", "Forbidden"));
    const auto first = session.createPage("first", "First").value();
    const auto second = session.createPage("second", "Second").value();
    const auto beforeRename = session.outline(second.metadata.id).value();
    const auto conflictingRename =
        session.renamePage(second.metadata.id, "first", "Conflict");
    REQUIRE_FALSE(conflictingRename);
    CHECK(conflictingRename.error().code ==
          hieda::notebook::NotebookErrorCode::pageNameConflict);
    CHECK(session.outline(second.metadata.id).value() == beforeRename);

    const auto revisionBeforeCreate =
        session.current().value_or(hieda::notebook::NotebookInfo{}).revision;
    hieda::notebook::NotebookSessionTestAccess::rejectNextCommit(session);
    const auto failedCreate = session.createPage("failed", "Failed");
    REQUIRE_FALSE(failedCreate);
    CHECK(failedCreate.error().code ==
          hieda::notebook::NotebookErrorCode::ioFailure);
    CHECK(
        session.current().value_or(hieda::notebook::NotebookInfo{}).revision ==
        revisionBeforeCreate);
    CHECK(session.pages().value().size() == 3);

    const auto beforeFailedRename = session.outline(first.metadata.id).value();
    hieda::notebook::NotebookSessionTestAccess::rejectNextCommit(session);
    const auto failedRename =
        session.renamePage(first.metadata.id, "renamed", "Renamed");
    REQUIRE_FALSE(failedRename);
    CHECK(session.outline(first.metadata.id).value() == beforeFailedRename);
    CHECK(session.createPage("renamed", "Available after rollback"));

    auto page =
        session.insertEntry(first.metadata.id, std::nullopt, "one").value();
    page = session
               .insertEntry(first.metadata.id, page.entries.front().metadata.id,
                            "two")
               .value();
    const auto acknowledged = page;
    const auto secondId = page.entries.back().metadata.id;
    const auto revisionBeforeMove =
        session.current().value_or(hieda::notebook::NotebookInfo{}).revision;
    hieda::notebook::NotebookSessionTestAccess::rejectNextCommit(session);
    const auto failedMove =
        session.moveEntry(secondId, hieda::notebook::EntryMove::indent, "two");
    REQUIRE_FALSE(failedMove);
    CHECK(session.outline(first.metadata.id).value() == acknowledged);
    CHECK(
        session.current().value_or(hieda::notebook::NotebookInfo{}).revision ==
        revisionBeforeMove);
    CHECK(session.editCapabilities().value().canUndo);

    hieda::notebook::NotebookSessionTestAccess::rejectNextCommit(session);
    const auto failedUndo = session.undoEdit();
    REQUIRE_FALSE(failedUndo);
    CHECK(session.outline(first.metadata.id).value() == acknowledged);
    CHECK(session.editCapabilities().value().canUndo);
    CHECK_FALSE(session.editCapabilities().value().canRedo);

    hieda::notebook::BlockId missingPage;
    missingPage.bytes.front() = std::byte{1};
    CHECK_FALSE(session.renamePage(missingPage, "missing", "Missing"));
    CHECK_FALSE(session.insertEntry(missingPage, std::nullopt, "missing"));
    CHECK(session.editCapabilities().value().canUndo);
}

TEST_CASE(
    "flat Journal Entries preserve identity Unicode text and insertion order")
{
    TemporaryDirectory temporaryDirectory;
    const auto notebookPath = temporaryDirectory.path() / "journal.hieda";
    const hieda::notebook::JournalDate date{2026, 8, 7};
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(notebookPath));

    const auto first = session.insertEntry(date, std::nullopt, "first");
    REQUIRE(first);
    REQUIRE(first.value().metadata);
    REQUIRE(first.value().entries.size() == 1);
    const auto firstId = first.value().entries.front().metadata.id;
    CHECK(first.value().entries.front().metadata.createdAt ==
          first.value().entries.front().metadata.updatedAt);

    const auto third = session.insertEntry(date, std::nullopt, "third");
    REQUIRE(third);
    REQUIRE(third.value().entries.size() == 2);
    const auto thirdId = third.value().entries.back().metadata.id;

    const std::string unicodeText = "\xE7\xAC\xAC\xE4\xBA\x8C \xF0\x9F\x8E\xB4";
    const auto second = session.insertEntry(date, firstId, unicodeText);
    REQUIRE(second);
    REQUIRE(second.value().entries.size() == 3);
    CHECK(second.value().entries[0].authoredText == "first");
    CHECK(second.value().entries[1].authoredText == unicodeText);
    CHECK(second.value().entries[2].metadata.id == thirdId);

    const auto& expected = second.value();
    session.close();
    REQUIRE(session.open(notebookPath));
    const auto reopened = session.outline(date);
    REQUIRE(reopened);
    CHECK(reopened.value() == expected);
}

TEST_CASE("editing a Journal Entry acknowledges exact multiline Unicode text")
{
    TemporaryDirectory temporaryDirectory;
    const auto notebookPath = temporaryDirectory.path() / "edit.hieda";
    const hieda::notebook::JournalDate date{2026, 8, 7};
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(notebookPath));
    const auto inserted = session.insertEntry(date, std::nullopt, "before");
    REQUIRE(inserted);
    const auto entry = inserted.value().entries.front();
    REQUIRE(inserted.value().metadata);
    const auto pageMetadata =
        inserted.value().metadata.value_or(hieda::notebook::BlockMetadata{});

    const auto updated = session.updateEntry(entry.metadata.id, "  after  ");
    REQUIRE(updated);
    CHECK(updated.value().authoredText == "  after  ");
    CHECK(updated.value().metadata.id == entry.metadata.id);
    CHECK(updated.value().metadata.createdAt == entry.metadata.createdAt);

    const std::string multiline =
        "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\nsecond line";
    const auto multilineUpdate =
        session.updateEntry(entry.metadata.id, multiline);
    REQUIRE(multilineUpdate);
    CHECK(multilineUpdate.value().authoredText == multiline);

    const auto rejected =
        session.updateEntry(entry.metadata.id, "carriage\rreturn");
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code ==
          hieda::notebook::NotebookErrorCode::invalidAuthoredText);
    const auto page = session.outline(date);
    REQUIRE(page);
    CHECK(page.value().entries.front().authoredText == multiline);
    CHECK(page.value().metadata == pageMetadata);

    session.close();
    REQUIRE(session.open(notebookPath));
    CHECK(session.outline(date).value().entries.front().authoredText ==
          multiline);
}

TEST_CASE(
    "Journal commands report closed sessions and invalid insertion points")
{
    const hieda::notebook::JournalDate date{2026, 8, 7};
    TemporaryDirectory temporaryDirectory;
    hieda::notebook::NotebookSession session;

    const auto closed = session.outline(date);
    REQUIRE_FALSE(closed);
    CHECK(closed.error().code ==
          hieda::notebook::NotebookErrorCode::notebookNotOpen);

    REQUIRE(session.create(temporaryDirectory.path() / "positions.hieda"));
    hieda::notebook::BlockId missing;
    missing.bytes.front() = std::byte{1};
    const auto invalid = session.insertEntry(date, missing, "entry");
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().code ==
          hieda::notebook::NotebookErrorCode::invalidInsertionPoint);
    const auto page = session.outline(date);
    REQUIRE(page);
    CHECK_FALSE(page.value().metadata.has_value());
}

TEST_CASE("a rejected Journal commit leaves the acknowledged state intact")
{
    TemporaryDirectory temporaryDirectory;
    const auto notebookPath = temporaryDirectory.path() / "rejected-save.hieda";
    const hieda::notebook::JournalDate date{2026, 8, 7};
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(notebookPath));
    const auto inserted = session.insertEntry(date, std::nullopt, "durable");
    REQUIRE(inserted);
    const auto entryId = inserted.value().entries.front().metadata.id;
    const auto revisionBeforeFailure =
        session.current().value_or(hieda::notebook::NotebookInfo{}).revision;

    hieda::notebook::NotebookSessionTestAccess::rejectNextCommit(session);
    const auto rejected = session.updateEntry(entryId, "not committed");

    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code ==
          hieda::notebook::NotebookErrorCode::ioFailure);
    CHECK(
        session.current().value_or(hieda::notebook::NotebookInfo{}).revision ==
        revisionBeforeFailure);
    const auto current = session.outline(date);
    REQUIRE(current);
    CHECK(current.value().entries.front().authoredText == "durable");
    session.close();
    REQUIRE(session.open(notebookPath));
    const auto reopened = session.outline(date);
    REQUIRE(reopened);
    CHECK(reopened.value().entries.front().authoredText == "durable");
}

TEST_CASE(
    "Journal ordering rebalances after repeated insertion at one position")
{
    TemporaryDirectory temporaryDirectory;
    const hieda::notebook::JournalDate date{2026, 8, 7};
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(temporaryDirectory.path() / "rebalance.hieda"));
    const auto first = session.insertEntry(date, std::nullopt, "anchor");
    REQUIRE(first);
    const auto anchor = first.value().entries.front().metadata.id;

    for (int index = 0; index < 40; ++index) {
        REQUIRE(session.insertEntry(date, anchor, std::to_string(index)));
    }

    const auto page = session.outline(date);
    REQUIRE(page);
    REQUIRE(page.value().entries.size() == 41);
    CHECK(page.value().entries.front().authoredText == "anchor");
    CHECK(page.value().entries[1].authoredText == "39");
    CHECK(page.value().entries.back().authoredText == "0");
}

TEST_CASE("subscribers observe committed Journal changes after the session "
          "lock is released")
{
    TemporaryDirectory temporaryDirectory;
    const hieda::notebook::JournalDate date{2026, 8, 7};
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(temporaryDirectory.path() / "subscription.hieda"));
    int notifications = 0;
    {
        auto subscription = session.subscribeToChanges([&]() -> void {
            ++notifications;
            REQUIRE(session.outline(date));
        });
        REQUIRE(session.insertEntry(date, std::nullopt, "committed"));
        CHECK(notifications == 1);
        const auto currentPage = session.outline(date);
        REQUIRE(currentPage);
        const auto entry = currentPage.value().entries.front();
        REQUIRE(session.updateEntry(entry.metadata.id, "changed"));
        CHECK(notifications == 2);
        REQUIRE(session.updateEntry(entry.metadata.id, "changed"));
        CHECK(notifications == 2);
    }
    REQUIRE(session.insertEntry(date, std::nullopt, "after unsubscribe"));
    CHECK(notifications == 2);
}

TEST_CASE("a failing subscriber cannot make a committed Journal command appear "
          "rejected")
{
    TemporaryDirectory temporaryDirectory;
    const hieda::notebook::JournalDate date{2026, 8, 7};
    hieda::notebook::NotebookSession session;
    REQUIRE(
        session.create(temporaryDirectory.path() / "subscriber-failure.hieda"));
    auto subscription = session.subscribeToChanges(
        []() -> void { throw std::runtime_error("observer failed"); });

    const auto inserted = session.insertEntry(date, std::nullopt, "committed");

    REQUIRE(inserted);
    CHECK(
        session.current().value_or(hieda::notebook::NotebookInfo{}).revision ==
        1);
    session.close();
    REQUIRE(
        session.open(temporaryDirectory.path() / "subscriber-failure.hieda"));
    REQUIRE(session.outline(date));
}

TEST_CASE("Journal Entry text rejects malformed UTF-8")
{
    TemporaryDirectory temporaryDirectory;
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(temporaryDirectory.path() / "utf8.hieda"));
    const std::string malformed{"\xC0\xAF", 2};

    const auto result = session.insertEntry(
        hieda::notebook::JournalDate{2026, 8, 7}, std::nullopt, malformed);

    REQUIRE_FALSE(result);
    CHECK(result.error().code ==
          hieda::notebook::NotebookErrorCode::invalidAuthoredText);
}

TEST_CASE(
    "nested Journal Entries preserve ancestry identity and order across reopen")
{
    TemporaryDirectory temporaryDirectory;
    const auto notebookPath = temporaryDirectory.path() / "nested.hieda";
    const hieda::notebook::JournalDate date{2026, 8, 8};
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(notebookPath));

    const auto insertedParent =
        session.insertEntry(date, std::nullopt, "parent");
    REQUIRE(insertedParent);
    const auto parentId = insertedParent.value().entries.front().metadata.id;
    const auto insertedChild =
        session.insertEntry(date, parentId, "child text");
    REQUIRE(insertedChild);
    const auto childId = insertedChild.value().entries.back().metadata.id;

    const auto indented = session.moveEntry(
        childId, hieda::notebook::EntryMove::indent, "child text");
    REQUIRE(indented);
    REQUIRE(indented.value().entries.size() == 2);
    CHECK(indented.value().entries[0].metadata.id == parentId);
    CHECK(indented.value().entries[1].metadata.id == childId);
    CHECK(indented.value().entries[1].parentEntry == parentId);

    const auto split = session.splitEntry(parentId, "parent", 3);
    REQUIRE(split);
    REQUIRE(split.value().entries.size() == 3);
    CHECK(split.value().entries[0].metadata.id == parentId);
    CHECK(split.value().entries[0].authoredText == "par");
    CHECK(split.value().entries[1].metadata.id == childId);
    CHECK(split.value().entries[1].parentEntry == parentId);
    CHECK(split.value().entries[2].authoredText == "ent");
    CHECK_FALSE(split.value().entries[2].parentEntry);

    const auto& expected = split.value();
    session.close();
    REQUIRE(session.open(notebookPath));
    const auto reopened = session.outline(date);
    REQUIRE(reopened);
    CHECK(reopened.value() == expected);
}

TEST_CASE("joining and deleting Entries enforce leaf-only structural changes")
{
    TemporaryDirectory temporaryDirectory;
    const hieda::notebook::JournalDate date{2026, 8, 8};
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(temporaryDirectory.path() / "join-delete.hieda"));
    const auto first = session.insertEntry(date, std::nullopt, "one");
    REQUIRE(first);
    const auto firstId = first.value().entries[0].metadata.id;
    const auto second = session.insertEntry(date, firstId, "two");
    REQUIRE(second);
    const auto secondId = second.value().entries[1].metadata.id;
    const auto third = session.insertEntry(date, secondId, "three");
    REQUIRE(third);
    const auto thirdId = third.value().entries[2].metadata.id;
    REQUIRE(session.moveEntry(thirdId, hieda::notebook::EntryMove::indent,
                              "three"));

    const auto rejectedJoin = session.joinEntry(secondId, "two");
    REQUIRE_FALSE(rejectedJoin);
    CHECK(rejectedJoin.error().code ==
          hieda::notebook::NotebookErrorCode::blockHasChildren);
    const auto rejectedDelete = session.deleteEntry(secondId);
    REQUIRE_FALSE(rejectedDelete);
    CHECK(rejectedDelete.error().code ==
          hieda::notebook::NotebookErrorCode::blockHasChildren);

    const auto joined = session.joinEntry(thirdId, "three");
    REQUIRE(joined);
    REQUIRE(joined.value().entries.size() == 2);
    CHECK(joined.value().entries[0].metadata.id == firstId);
    CHECK(joined.value().entries[1].metadata.id == secondId);
    CHECK(joined.value().entries[1].authoredText == "twothree");

    const auto deleted = session.deleteEntry(secondId);
    REQUIRE(deleted);
    REQUIRE(deleted.value().entries.size() == 1);
    CHECK(deleted.value().entries.front().metadata.id == firstId);
}

TEST_CASE("local moves reorder complete Journal subtrees")
{
    TemporaryDirectory temporaryDirectory;
    const hieda::notebook::JournalDate date{2026, 8, 8};
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(temporaryDirectory.path() / "moves.hieda"));
    REQUIRE(session.insertEntry(date, std::nullopt, "A"));
    auto page = session.insertEntry(date, std::nullopt, "B");
    REQUIRE(page);
    page = session.insertEntry(date, std::nullopt, "C");
    REQUIRE(page);
    const auto firstId = page.value().entries[0].metadata.id;
    const auto secondId = page.value().entries[1].metadata.id;
    const auto thirdId = page.value().entries[2].metadata.id;

    REQUIRE(
        session.moveEntry(secondId, hieda::notebook::EntryMove::indent, "B"));
    REQUIRE(
        session.moveEntry(thirdId, hieda::notebook::EntryMove::indent, "C"));
    page = session.insertEntry(date, secondId, "B child");
    REQUIRE(page);
    const auto bChild = page.value().entries[2].metadata.id;
    REQUIRE(session.moveEntry(bChild, hieda::notebook::EntryMove::indent,
                              "B child"));

    const auto movedDown =
        session.moveEntry(secondId, hieda::notebook::EntryMove::down, "B");
    REQUIRE(movedDown);
    REQUIRE(movedDown.value().entries.size() == 4);
    CHECK(movedDown.value().entries[0].metadata.id == firstId);
    CHECK(movedDown.value().entries[1].metadata.id == thirdId);
    CHECK(movedDown.value().entries[2].metadata.id == secondId);
    CHECK(movedDown.value().entries[3].metadata.id == bChild);
    CHECK(movedDown.value().entries[3].parentEntry == secondId);

    const auto movedUp =
        session.moveEntry(secondId, hieda::notebook::EntryMove::up, "B");
    REQUIRE(movedUp);
    CHECK(movedUp.value().entries[1].metadata.id == secondId);
    CHECK(movedUp.value().entries[2].metadata.id == bChild);
    CHECK(movedUp.value().entries[3].metadata.id == thirdId);

    const auto outdented =
        session.moveEntry(secondId, hieda::notebook::EntryMove::outdent, "B");
    REQUIRE(outdented);
    CHECK(outdented.value().entries[0].metadata.id == firstId);
    CHECK(outdented.value().entries[1].metadata.id == thirdId);
    CHECK(outdented.value().entries[1].parentEntry == firstId);
    CHECK(outdented.value().entries[2].metadata.id == secondId);
    CHECK_FALSE(outdented.value().entries[2].parentEntry);
    CHECK(outdented.value().entries[3].parentEntry == secondId);
}

TEST_CASE(
    "invalid and failed structural edits leave the acknowledged outline intact")
{
    TemporaryDirectory temporaryDirectory;
    const auto notebookPath =
        temporaryDirectory.path() / "atomic-outline.hieda";
    const hieda::notebook::JournalDate date{2026, 8, 8};
    const std::string emojiText = "A \xF0\x9F\x8E\xB4 B";
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(notebookPath));
    const auto inserted = session.insertEntry(date, std::nullopt, emojiText);
    REQUIRE(inserted);
    const auto entryId = inserted.value().entries.front().metadata.id;
    const auto& acknowledged = inserted.value();
    const auto revision =
        session.current().value_or(hieda::notebook::NotebookInfo{}).revision;

    const auto invalidCursor = session.splitEntry(entryId, emojiText, 3);
    REQUIRE_FALSE(invalidCursor);
    CHECK(invalidCursor.error().code ==
          hieda::notebook::NotebookErrorCode::invalidCursorPosition);
    const auto invalidMove =
        session.moveEntry(entryId, hieda::notebook::EntryMove::up, "changed");
    REQUIRE_FALSE(invalidMove);
    CHECK(invalidMove.error().code ==
          hieda::notebook::NotebookErrorCode::invalidStructuralMove);
    CHECK(
        session.current().value_or(hieda::notebook::NotebookInfo{}).revision ==
        revision);
    REQUIRE(session.outline(date).value() == acknowledged);

    hieda::notebook::NotebookSessionTestAccess::rejectNextCommit(session);
    const auto failedSplit = session.splitEntry(entryId, emojiText, 2);
    REQUIRE_FALSE(failedSplit);
    CHECK(failedSplit.error().code ==
          hieda::notebook::NotebookErrorCode::ioFailure);
    CHECK(
        session.current().value_or(hieda::notebook::NotebookInfo{}).revision ==
        revision);
    CHECK(session.outline(date).value() == acknowledged);
    session.close();
    REQUIRE(session.open(notebookPath));
    CHECK(session.outline(date).value() == acknowledged);
}

TEST_CASE(
    "multiline Journal Entries split exactly at Unicode cursor boundaries")
{
    TemporaryDirectory temporaryDirectory;
    const hieda::notebook::JournalDate date{2026, 8, 8};
    hieda::notebook::NotebookSession session;
    REQUIRE(
        session.create(temporaryDirectory.path() / "multiline-split.hieda"));
    const std::string text = "first line\nA \xF0\x9F\x8E\xB4 second line";
    const auto inserted = session.insertEntry(date, std::nullopt, text);
    REQUIRE(inserted);
    const auto entryId = inserted.value().entries.front().metadata.id;
    const auto splitOffset = text.find(" second");

    const auto split = session.splitEntry(entryId, text, splitOffset);

    REQUIRE(split);
    REQUIRE(split.value().entries.size() == 2);
    CHECK(split.value().entries[0].authoredText ==
          "first line\nA \xF0\x9F\x8E\xB4");
    CHECK(split.value().entries[1].authoredText == " second line");
}

TEST_CASE("selected Journal subtrees are cut as one durable undoable action")
{
    TemporaryDirectory temporaryDirectory;
    const auto notebookPath = temporaryDirectory.path() / "cut-subtrees.hieda";
    const hieda::notebook::JournalDate date{2026, 8, 8};
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(notebookPath));
    REQUIRE(session.insertEntry(date, std::nullopt, "parent"));
    auto page = session.insertEntry(date, std::nullopt, "child");
    REQUIRE(page);
    page = session.insertEntry(date, std::nullopt, "tail");
    REQUIRE(page);
    const auto parentId = page.value().entries[0].metadata.id;
    const auto childId = page.value().entries[1].metadata.id;
    const auto tailId = page.value().entries[2].metadata.id;
    REQUIRE(session.moveEntry(childId, hieda::notebook::EntryMove::indent,
                              "child"));
    const auto before = session.outline(date).value();

    const auto cut = session.deleteSubtrees({parentId, childId});

    REQUIRE(cut);
    REQUIRE(cut.value().entries.size() == 1);
    CHECK(cut.value().entries.front().metadata.id == tailId);
    CHECK(session.undoEdit().value().front() == before);
    CHECK(session.redoEdit().value().front() == cut.value());
    session.close();
    REQUIRE(session.open(notebookPath));
    CHECK(session.outline(date).value() == cut.value());
}

TEST_CASE(
    "failed Journal subtree deletion preserves content revision and history")
{
    TemporaryDirectory temporaryDirectory;
    const hieda::notebook::JournalDate date{2026, 8, 8};
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(temporaryDirectory.path() /
                           "failed-cut-subtrees.hieda"));
    const auto page = session.insertEntry(date, std::nullopt, "kept").value();
    const auto entryId = page.entries.front().metadata.id;
    const auto revision =
        session.current().value_or(hieda::notebook::NotebookInfo{}).revision;
    hieda::notebook::NotebookSessionTestAccess::rejectNextCommit(session);

    const auto failed = session.deleteSubtrees({entryId});

    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == hieda::notebook::NotebookErrorCode::ioFailure);
    CHECK(
        session.current().value_or(hieda::notebook::NotebookInfo{}).revision ==
        revision);
    CHECK(session.outline(date).value() == page);
    CHECK(session.editCapabilities().value().canUndo);
    CHECK_FALSE(session.editCapabilities().value().canRedo);
}

TEST_CASE("Journal subtree deletion normalizes duplicates and rejects invalid "
          "selections")
{
    TemporaryDirectory temporaryDirectory;
    const hieda::notebook::JournalDate firstDate{2026, 8, 8};
    const hieda::notebook::JournalDate secondDate{2026, 8, 9};
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(temporaryDirectory.path() /
                           "subtree-selection-validation.hieda"));
    REQUIRE(session.insertEntry(firstDate, std::nullopt, "parent"));
    auto firstPage =
        session.insertEntry(firstDate, std::nullopt, "child").value();
    const auto parentId = firstPage.entries[0].metadata.id;
    const auto childId = firstPage.entries[1].metadata.id;
    REQUIRE(session.moveEntry(childId, hieda::notebook::EntryMove::indent,
                              "child"));
    const auto foreignPage =
        session.insertEntry(secondDate, std::nullopt, "foreign").value();
    const auto foreignId = foreignPage.entries.front().metadata.id;
    firstPage = session.outline(firstDate).value();

    const auto duplicateCut = session.deleteSubtrees({childId, childId});
    REQUIRE(duplicateCut);
    REQUIRE(duplicateCut.value().entries.size() == 1);
    CHECK(duplicateCut.value().entries.front().metadata.id == parentId);
    REQUIRE(session.undoEdit());
    CHECK(session.outline(firstDate).value() == firstPage);

    const auto revision =
        session.current().value_or(hieda::notebook::NotebookInfo{}).revision;
    const auto empty = session.deleteSubtrees({});
    REQUIRE_FALSE(empty);
    CHECK(empty.error().code ==
          hieda::notebook::NotebookErrorCode::invalidStructuralMove);
    hieda::notebook::BlockId missingId;
    missingId.bytes.front() = std::byte{1};
    const auto missing = session.deleteSubtrees({parentId, missingId});
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code ==
          hieda::notebook::NotebookErrorCode::blockNotFound);
    const auto crossPage = session.deleteSubtrees({parentId, foreignId});
    REQUIRE_FALSE(crossPage);
    CHECK(crossPage.error().code ==
          hieda::notebook::NotebookErrorCode::blockNotFound);
    CHECK(
        session.current().value_or(hieda::notebook::NotebookInfo{}).revision ==
        revision);
    CHECK(session.outline(firstDate).value() == firstPage);
    CHECK(session.outline(secondDate).value() == foreignPage);

    const auto emptied = session.deleteSubtrees({parentId});
    REQUIRE(emptied);
    CHECK(emptied.value().metadata.has_value());
    CHECK(emptied.value().entries.empty());
    CHECK(session.undoEdit().value().front() == firstPage);
}

TEST_CASE("Journal structural commands match a reference outline model")
{
    struct ReferenceEntry {
        hieda::notebook::BlockId id;
        std::string text;
        std::optional<hieda::notebook::BlockId> parent;
    };

    TemporaryDirectory temporaryDirectory;
    const hieda::notebook::JournalDate date{2026, 8, 8};
    hieda::notebook::NotebookSession session;
    REQUIRE(
        session.create(temporaryDirectory.path() / "reference-model.hieda"));
    REQUIRE(session.insertEntry(date, std::nullopt, "A"));
    REQUIRE(session.insertEntry(date, std::nullopt, "Bee"));
    REQUIRE(session.insertEntry(date, std::nullopt, "C"));
    auto actual = session.insertEntry(date, std::nullopt, "D");
    REQUIRE(actual);
    const auto firstId = actual.value().entries[0].metadata.id;
    const auto secondId = actual.value().entries[1].metadata.id;
    const auto thirdId = actual.value().entries[2].metadata.id;
    const auto fourthId = actual.value().entries[3].metadata.id;
    std::vector<ReferenceEntry> expected{{firstId, "A", std::nullopt},
                                         {secondId, "Bee", std::nullopt},
                                         {thirdId, "C", std::nullopt},
                                         {fourthId, "D", std::nullopt}};
    const auto checkModel =
        [&](const hieda::notebook::OutlinePage& page) -> void {
        REQUIRE(page.entries.size() == expected.size());
        for (std::size_t index = 0; index < expected.size(); ++index) {
            CHECK(page.entries[index].metadata.id == expected[index].id);
            CHECK(page.entries[index].authoredText == expected[index].text);
            CHECK(page.entries[index].parentEntry == expected[index].parent);
        }
    };
    checkModel(actual.value());

    actual =
        session.moveEntry(secondId, hieda::notebook::EntryMove::indent, "Bee");
    REQUIRE(actual);
    expected[1].parent = firstId;
    checkModel(actual.value());

    actual =
        session.moveEntry(thirdId, hieda::notebook::EntryMove::indent, "C");
    REQUIRE(actual);
    expected[2].parent = firstId;
    checkModel(actual.value());
    actual =
        session.moveEntry(thirdId, hieda::notebook::EntryMove::indent, "C");
    REQUIRE(actual);
    expected[2].parent = secondId;
    checkModel(actual.value());

    actual = session.splitEntry(secondId, "Bee", 1);
    REQUIRE(actual);
    const auto splitId = actual.value().entries[3].metadata.id;
    expected[1].text = "B";
    expected.insert(expected.begin() + 3, {splitId, "ee", firstId});
    checkModel(actual.value());

    actual = session.joinEntry(splitId, "ee");
    REQUIRE(actual);
    expected[2].text = "Cee";
    expected.erase(expected.begin() + 3);
    checkModel(actual.value());

    actual =
        session.moveEntry(thirdId, hieda::notebook::EntryMove::outdent, "Cee");
    REQUIRE(actual);
    expected[2].parent = firstId;
    checkModel(actual.value());
    actual = session.moveEntry(thirdId, hieda::notebook::EntryMove::up, "Cee");
    REQUIRE(actual);
    std::swap(expected[1], expected[2]);
    checkModel(actual.value());
    actual =
        session.moveEntry(thirdId, hieda::notebook::EntryMove::down, "Cee");
    REQUIRE(actual);
    std::swap(expected[1], expected[2]);
    checkModel(actual.value());

    actual = session.deleteEntry(thirdId);
    REQUIRE(actual);
    expected.erase(expected.begin() + 2);
    checkModel(actual.value());

    const auto revision =
        session.current().value_or(hieda::notebook::NotebookInfo{}).revision;
    hieda::notebook::NotebookSessionTestAccess::rejectNextCommit(session);
    const auto failedMove =
        session.moveEntry(fourthId, hieda::notebook::EntryMove::up, "dirty D");
    REQUIRE_FALSE(failedMove);
    CHECK(failedMove.error().code ==
          hieda::notebook::NotebookErrorCode::ioFailure);
    CHECK(
        session.current().value_or(hieda::notebook::NotebookInfo{}).revision ==
        revision);
    checkModel(session.outline(date).value());

    const auto rejectedParentDelete = session.deleteEntry(firstId);
    REQUIRE_FALSE(rejectedParentDelete);
    CHECK(rejectedParentDelete.error().code ==
          hieda::notebook::NotebookErrorCode::blockHasChildren);
    const auto rejectedOutdent = session.moveEntry(
        fourthId, hieda::notebook::EntryMove::outdent, "dirty D");
    REQUIRE_FALSE(rejectedOutdent);
    CHECK(rejectedOutdent.error().code ==
          hieda::notebook::NotebookErrorCode::invalidStructuralMove);
    CHECK(
        session.current().value_or(hieda::notebook::NotebookInfo{}).revision ==
        revision);
    checkModel(session.outline(date).value());
}

TEST_CASE(
    "generated Journal moves preserve preorder and single-parent properties")
{
    TemporaryDirectory temporaryDirectory;
    const auto notebookPath =
        temporaryDirectory.path() / "generated-outline.hieda";
    const hieda::notebook::JournalDate date{2026, 8, 8};
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(notebookPath));
    for (int index = 0; index < 12; ++index) {
        REQUIRE(session.insertEntry(date, std::nullopt, std::to_string(index)));
    }
    auto page = session.outline(date).value();
    for (std::size_t index = 1; index < page.entries.size(); index += 2) {
        const auto result = session.moveEntry(
            page.entries[index].metadata.id, hieda::notebook::EntryMove::indent,
            page.entries[index].authoredText);
        REQUIRE(result);
        page = result.value();
    }
    for (std::size_t operation = 0; operation < 20; ++operation) {
        const auto row = operation % page.entries.size();
        const auto movement = operation % 2 == 0
                                  ? hieda::notebook::EntryMove::down
                                  : hieda::notebook::EntryMove::up;
        const auto result =
            session.moveEntry(page.entries[row].metadata.id, movement,
                              page.entries[row].authoredText);
        if (result) {
            page = result.value();
        }
        for (std::size_t entryIndex = 0; entryIndex < page.entries.size();
             ++entryIndex) {
            const auto& entry = page.entries[entryIndex];
            CHECK(std::count_if(page.entries.begin(), page.entries.end(),
                                [&](const auto& candidate) -> bool {
                                    return candidate.metadata.id ==
                                           entry.metadata.id;
                                }) == 1);
            if (entry.parentEntry) {
                const auto parent = std::ranges::find_if(
                    page.entries, [&](const auto& candidate) -> bool {
                        return candidate.metadata.id == *entry.parentEntry;
                    });
                REQUIRE(parent != page.entries.end());
                CHECK(std::cmp_less(std::distance(page.entries.begin(), parent),
                                    entryIndex));
            }
        }
    }
    session.close();
    REQUIRE(session.open(notebookPath));
    CHECK(session.outline(date).value() == page);
}

TEST_CASE("Journal edits undo and redo as coherent user actions")
{
    TemporaryDirectory temporaryDirectory;
    hieda::notebook::NotebookSession session;
    const hieda::notebook::JournalDate date{2026, 8, 8};
    REQUIRE(session.create(temporaryDirectory.path() / "undo-redo.hieda"));

    CHECK(session.editCapabilities().value() ==
          hieda::notebook::EditCapabilities{});
    auto page = session.insertEntry(date, std::nullopt, "parent").value();
    const auto parentId = page.entries.front().metadata.id;
    page = session.insertEntry(date, parentId, "child").value();
    const auto childId = page.entries.back().metadata.id;
    page = session
               .moveEntry(childId, hieda::notebook::EntryMove::indent,
                          "edited child")
               .value();
    const auto acknowledged = page;

    REQUIRE(session.undoEdit());
    page = session.outline(date).value();
    REQUIRE(page.entries.size() == 2);
    CHECK(page.entries[1].authoredText == "child");
    CHECK_FALSE(page.entries[1].parentEntry);

    REQUIRE(session.redoEdit());
    CHECK(session.outline(date).value() == acknowledged);
}

TEST_CASE("every supported Journal command round-trips through history")
{
    TemporaryDirectory temporaryDirectory;
    hieda::notebook::NotebookSession session;
    const hieda::notebook::JournalDate date{2026, 8, 8};
    REQUIRE(session.create(temporaryDirectory.path() /
                           "all-history-actions.hieda"));
    std::vector<hieda::notebook::OutlinePage> states;
    states.push_back(session.outline(date).value());
    const auto remember =
        [&](hieda::notebook::OutlinePage page) -> hieda::notebook::OutlinePage {
        states.push_back(page);
        return page;
    };

    auto page = remember(session.insertEntry(date, std::nullopt, "A").value());
    const auto firstId = page.entries[0].metadata.id;
    page = remember(session.insertEntry(date, std::nullopt, "BC").value());
    const auto secondId = page.entries[1].metadata.id;
    page = remember(session.insertEntry(date, std::nullopt, "D").value());
    const auto thirdId = page.entries[2].metadata.id;
    REQUIRE(session.updateEntry(secondId, "B2C"));
    page = remember(session.outline(date).value());
    page = remember(session.splitEntry(secondId, "B2C", 2).value());
    const auto splitId = page.entries[2].metadata.id;
    page = remember(session.joinEntry(splitId, "2C").value());
    page = remember(
        session.moveEntry(secondId, hieda::notebook::EntryMove::indent, "B2C")
            .value());
    page = remember(
        session.moveEntry(secondId, hieda::notebook::EntryMove::outdent, "B2C")
            .value());
    page = remember(
        session.moveEntry(secondId, hieda::notebook::EntryMove::down, "B2C")
            .value());
    page = remember(
        session.moveEntry(secondId, hieda::notebook::EntryMove::up, "B2C")
            .value());
    page = remember(session.deleteEntry(thirdId).value());
    CHECK(page.entries.front().metadata.id == firstId);

    for (std::size_t index = states.size() - 1; index > 0; --index) {
        CHECK(session.undoEdit().value().front() == states[index - 1]);
    }
    CHECK_FALSE(session.editCapabilities().value().canUndo);
    for (std::size_t index = 1; index < states.size(); ++index) {
        CHECK(session.redoEdit().value().front() == states[index]);
    }
    CHECK_FALSE(session.editCapabilities().value().canRedo);
}

TEST_CASE("undo restores deleted identity and redo branches clear only after "
          "committed edits")
{
    TemporaryDirectory temporaryDirectory;
    hieda::notebook::NotebookSession session;
    const hieda::notebook::JournalDate date{2026, 8, 8};
    REQUIRE(session.create(temporaryDirectory.path() / "undo-delete.hieda"));
    auto page = session.insertEntry(date, std::nullopt, "kept").value();
    const auto entry = page.entries.front();
    REQUIRE(session.deleteEntry(entry.metadata.id));

    page = session.undoEdit().value().front();
    REQUIRE(page.entries.size() == 1);
    CHECK(page.entries.front() == entry);
    CHECK(session.editCapabilities().value().canRedo);

    const auto rejected =
        session.updateEntry(entry.metadata.id, "carriage\rreturn");
    REQUIRE_FALSE(rejected);
    CHECK(session.editCapabilities().value().canRedo);
    REQUIRE(session.updateEntry(entry.metadata.id, entry.authoredText));
    CHECK(session.editCapabilities().value().canRedo);
    REQUIRE(session.updateEntry(entry.metadata.id, "changed"));
    CHECK_FALSE(session.editCapabilities().value().canRedo);
}

TEST_CASE("failed undo leaves acknowledged content revision and history intact")
{
    TemporaryDirectory temporaryDirectory;
    hieda::notebook::NotebookSession session;
    const hieda::notebook::JournalDate date{2026, 8, 8};
    REQUIRE(session.create(temporaryDirectory.path() / "failed-undo.hieda"));
    const auto page =
        session.insertEntry(date, std::nullopt, "durable").value();
    const auto revision =
        session.current().value_or(hieda::notebook::NotebookInfo{}).revision;
    hieda::notebook::NotebookSessionTestAccess::rejectNextCommit(session);

    const auto failed = session.undoEdit();

    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == hieda::notebook::NotebookErrorCode::ioFailure);
    CHECK(session.outline(date).value() == page);
    CHECK(
        session.current().value_or(hieda::notebook::NotebookInfo{}).revision ==
        revision);
    CHECK(session.editCapabilities().value().canUndo);
    CHECK_FALSE(session.editCapabilities().value().canRedo);
}

TEST_CASE(
    "Notebook history is chronological and clears when the Notebook closes")
{
    TemporaryDirectory temporaryDirectory;
    const auto path = temporaryDirectory.path() / "page-history.hieda";
    hieda::notebook::NotebookSession session;
    const hieda::notebook::JournalDate firstDate{2026, 8, 8};
    const hieda::notebook::JournalDate secondDate{2026, 8, 9};
    REQUIRE(session.create(path));
    const auto first =
        session.insertEntry(firstDate, std::nullopt, "first").value();
    const auto second =
        session.insertEntry(secondDate, std::nullopt, "second").value();

    const auto virtualPage = session.undoEdit().value().front();
    CHECK_FALSE(virtualPage.metadata);
    CHECK(virtualPage.entries.empty());
    CHECK(virtualPage.journalDate == secondDate);
    CHECK(session.outline(firstDate).value() == first);
    CHECK(session.outline(secondDate).value().entries.empty());
    CHECK(session.editCapabilities().value().canRedo);
    CHECK(session.editCapabilities().value().canUndo);
    const auto restored = session.redoEdit().value().front();
    CHECK(restored == second);

    session.close();
    REQUIRE(session.open(path));
    CHECK(session.editCapabilities().value() ==
          hieda::notebook::EditCapabilities{});
    const auto unavailable = session.undoEdit();
    REQUIRE_FALSE(unavailable);
    CHECK(unavailable.error().code ==
          hieda::notebook::NotebookErrorCode::undoUnavailable);
}

TEST_CASE("failed redo preserves the undone state and redo capability")
{
    TemporaryDirectory temporaryDirectory;
    hieda::notebook::NotebookSession session;
    const hieda::notebook::JournalDate date{2026, 8, 8};
    REQUIRE(session.create(temporaryDirectory.path() / "failed-redo.hieda"));
    const auto committed =
        session.insertEntry(date, std::nullopt, "durable").value();
    const auto undone = session.undoEdit().value().front();
    const auto revision =
        session.current().value_or(hieda::notebook::NotebookInfo{}).revision;
    hieda::notebook::NotebookSessionTestAccess::rejectNextCommit(session);

    const auto failed = session.redoEdit();

    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == hieda::notebook::NotebookErrorCode::ioFailure);
    CHECK(session.outline(date).value() == undone);
    CHECK(
        session.current().value_or(hieda::notebook::NotebookInfo{}).revision ==
        revision);
    CHECK(session.editCapabilities().value().canRedo);
    CHECK(session.redoEdit().value().front() == committed);
}

TEST_CASE("Journal history evicts old actions under its memory budget")
{
    TemporaryDirectory temporaryDirectory;
    hieda::notebook::NotebookSession session;
    const hieda::notebook::JournalDate date{2026, 8, 8};
    REQUIRE(
        session.create(temporaryDirectory.path() / "bounded-history.hieda"));
    const std::string firstText(1024ULL * 1024ULL, 'a');
    const auto inserted =
        session.insertEntry(date, std::nullopt, firstText).value();
    const auto id = inserted.entries.front().metadata.id;
    for (int index = 0; index < 20; ++index) {
        REQUIRE(session.updateEntry(
            id, std::string(1024ULL * 1024ULL, index % 2 == 0 ? 'b' : 'c')));
    }

    const auto restored = session.undoEdit().value().front();
    REQUIRE(restored.entries.size() == 1);
    CHECK(restored.entries.front().authoredText ==
          std::string(1024ULL * 1024ULL, 'b'));
    auto undoCount = std::size_t{1};
    while (session.editCapabilities().value().canUndo) {
        REQUIRE(session.undoEdit());
        ++undoCount;
    }
    CHECK(undoCount < 21);
    CHECK(session.editCapabilities().value().canRedo);
}

TEST_CASE("ordinary Page history shares the Journal memory budget")
{
    TemporaryDirectory temporaryDirectory;
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(temporaryDirectory.path() /
                           "bounded-page-history.hieda"));
    const auto page = session.createPage("bounded", "Bounded").value();
    const hieda::notebook::JournalDate date{2026, 8, 8};
    const std::string firstText(1024ULL * 1024ULL, 'a');
    REQUIRE(session.insertEntry(date, std::nullopt, firstText));
    CHECK(session.editCapabilities().value().canUndo);
    const auto inserted =
        session.insertEntry(page.metadata.id, std::nullopt, firstText).value();
    const auto id = inserted.entries.front().metadata.id;
    CHECK(session.editCapabilities().value().canUndo);
    for (int index = 0; index < 20; ++index) {
        REQUIRE(session.updateEntry(
            id, std::string(1024ULL * 1024ULL, index % 2 == 0 ? 'b' : 'c')));
    }

    const auto restored = session.undoEdit().value().front();
    REQUIRE(restored.entries.size() == 1);
    CHECK(restored.entries.front().authoredText ==
          std::string(1024ULL * 1024ULL, 'b'));
    auto undoCount = std::size_t{1};
    while (session.editCapabilities().value().canUndo) {
        REQUIRE(session.undoEdit());
        ++undoCount;
    }
    CHECK(undoCount < 23);
    CHECK(session.editCapabilities().value().canRedo);
}

TEST_CASE("schema v2 persists Page kind separately from one Entry type")
{
    TemporaryDirectory temporaryDirectory;
    const auto notebookPath =
        temporaryDirectory.path() / "unified-blocks.hieda";
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(notebookPath));
    const auto named = session.createPage("schema", "Schema").value();
    const auto payloadPage = session.createPage("payload", "Payload").value();
    const hieda::notebook::JournalDate date{2026, 8, 9};
    const auto journal =
        session.insertEntry(date, std::nullopt, "entry").value();
    REQUIRE(journal.metadata);
    const auto journalMetadata =
        journal.metadata.value_or(hieda::notebook::BlockMetadata{});
    const auto entryId = journal.entries.front().metadata.id;
    session.close();

    const auto namedRecord = readBlockRecord(notebookPath, named.metadata.id);
    const auto payloadPageRecord =
        readBlockRecord(notebookPath, payloadPage.metadata.id);
    const auto journalRecord =
        readBlockRecord(notebookPath, journalMetadata.id);
    const auto entryRecord = readBlockRecord(notebookPath, entryId);
    REQUIRE(namedRecord.size() >= 2);
    REQUIRE(journalRecord.size() >= 2);
    REQUIRE(entryRecord.size() >= 2);
    CHECK(namedRecord[1] == 2);
    CHECK(journalRecord[1] == 2);
    CHECK(entryRecord[1] == 2);
    REQUIRE(namedRecord.size() > 8);
    REQUIRE(journalRecord.size() > 8);
    REQUIRE(entryRecord.size() > 8);
    CHECK(namedRecord[8] == 1);
    CHECK(journalRecord[8] == 1);
    CHECK(entryRecord[8] == 2);
    const auto namedTags = blockRecordTags(namedRecord);
    const auto journalTags = blockRecordTags(journalRecord);
    const auto entryTags = blockRecordTags(entryRecord);
    CHECK(std::ranges::find(namedTags, 4) != namedTags.end());
    CHECK(std::ranges::find(journalTags, 4) != journalTags.end());
    CHECK(std::ranges::find(entryTags, 4) == entryTags.end());
    CHECK(std::ranges::find(entryTags, 6) != entryTags.end());

    auto invalidNamedRecord = namedRecord;
    std::vector<std::uint8_t> packedDate;
    appendU32(packedDate, 20260809);
    appendField(invalidNamedRecord, 5, packedDate);
    writeBlockRecord(notebookPath, named.metadata.id,
                     std::move(invalidNamedRecord));
    auto invalidPayloadRecord = payloadPageRecord;
    appendField(invalidPayloadRecord, 6, std::vector<std::uint8_t>{'x'});
    writeBlockRecord(notebookPath, payloadPage.metadata.id,
                     std::move(invalidPayloadRecord));
    auto invalidJournalRecord = journalRecord;
    std::vector<std::uint8_t> invalidPackedDate;
    appendU32(invalidPackedDate, 20261340);
    appendField(invalidJournalRecord, 5, invalidPackedDate);
    writeBlockRecord(notebookPath, journalMetadata.id,
                     std::move(invalidJournalRecord));

    REQUIRE(session.open(notebookPath));
    const auto invalid = session.outline(named.metadata.id);
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().code ==
          hieda::notebook::NotebookErrorCode::invalidNotebook);
    const auto invalidPayload = session.outline(payloadPage.metadata.id);
    REQUIRE_FALSE(invalidPayload);
    CHECK(invalidPayload.error().code ==
          hieda::notebook::NotebookErrorCode::invalidNotebook);
    const auto invalidJournal = session.outline(date);
    REQUIRE_FALSE(invalidJournal);
    CHECK(invalidJournal.error().code ==
          hieda::notebook::NotebookErrorCode::invalidNotebook);
}

TEST_CASE("Notebook history is chronological across Page kinds")
{
    TemporaryDirectory temporaryDirectory;
    hieda::notebook::NotebookSession session;
    REQUIRE(
        session.create(temporaryDirectory.path() / "notebook-history.hieda"));
    const auto named = session.createPage("history", "History").value();
    const hieda::notebook::JournalDate date{2026, 8, 9};
    REQUIRE(session.insertEntry(named.metadata.id, std::nullopt, "named"));
    REQUIRE(session.insertEntry(date, std::nullopt, "journal"));

    REQUIRE(session.undoEdit());
    CHECK(session.outline(date).value().entries.empty());
    REQUIRE(session.outline(named.metadata.id).value().entries.size() == 1);
    REQUIRE(session.undoEdit());
    CHECK(session.outline(named.metadata.id).value().entries.empty());

    REQUIRE(session.redoEdit());
    REQUIRE(session.outline(named.metadata.id).value().entries.size() == 1);
    REQUIRE(session.redoEdit());
    REQUIRE(session.outline(date).value().entries.size() == 1);

    REQUIRE(session.undoEdit());
    REQUIRE(session.updateEntry(
        session.outline(named.metadata.id).value().entries.front().metadata.id,
        "new branch"));
    CHECK_FALSE(session.editCapabilities().value().canRedo);
    const auto unavailable = session.redoEdit();
    REQUIRE_FALSE(unavailable);
    CHECK(unavailable.error().code ==
          hieda::notebook::NotebookErrorCode::redoUnavailable);
}

TEST_CASE("an Entry subtree moves atomically between Named and Journal Pages")
{
    TemporaryDirectory temporaryDirectory;
    hieda::notebook::NotebookSession session;
    REQUIRE(
        session.create(temporaryDirectory.path() / "cross-page-move.hieda"));
    const auto named = session.createPage("ideas", "Ideas").value();
    const auto namedId = named.metadata.id;
    auto source = session.insertEntry(namedId, std::nullopt, "parent").value();
    const auto parent = source.entries.front();
    source = session.insertEntry(namedId, parent.metadata.id, "child").value();
    const auto child = source.entries.back();
    source =
        session
            .moveEntry(child.metadata.id, hieda::notebook::EntryMove::indent,
                       child.authoredText)
            .value();
    const auto destinationDate = hieda::notebook::JournalDate{2026, 8, 9};

    const auto moved = session.moveEntryToPage(parent.metadata.id,
                                               destinationDate, std::nullopt);

    REQUIRE(moved);
    REQUIRE(moved.value().size() == 2);
    const auto emptiedSource = session.outline(namedId);
    REQUIRE(emptiedSource);
    CHECK(emptiedSource.value().entries.empty());
    const auto destinationResult = session.outline(destinationDate);
    REQUIRE(destinationResult);
    const auto& destination = destinationResult.value();
    REQUIRE(destination.entries.size() == 2);
    CHECK(destination.kind == hieda::notebook::PageKind::journal);
    CHECK(destination.entries[0].metadata.id == parent.metadata.id);
    CHECK(destination.entries[0].metadata.createdAt ==
          parent.metadata.createdAt);
    CHECK(destination.entries[0].authoredText == parent.authoredText);
    CHECK(destination.entries[1].metadata.id == child.metadata.id);
    CHECK(destination.entries[1].metadata.createdAt ==
          child.metadata.createdAt);
    CHECK(destination.entries[1].parentEntry == parent.metadata.id);

    REQUIRE(session.undoEdit());
    CHECK(session.outline(namedId).value().entries == source.entries);
    CHECK(session.outline(destinationDate).value().entries.empty());
    REQUIRE(session.redoEdit());
    CHECK(session.outline(namedId).value().entries.empty());
    CHECK(session.outline(destinationDate).value().entries ==
          destination.entries);

    const auto revision =
        session.current().value_or(hieda::notebook::NotebookInfo{}).revision;
    hieda::notebook::NotebookSessionTestAccess::rejectNextCommit(session);
    const auto failedMove =
        session.moveEntryToPage(parent.metadata.id, namedId, std::nullopt);
    REQUIRE_FALSE(failedMove);
    CHECK(failedMove.error().code ==
          hieda::notebook::NotebookErrorCode::ioFailure);
    CHECK(
        session.current().value_or(hieda::notebook::NotebookInfo{}).revision ==
        revision);
    CHECK(session.outline(namedId).value().entries.empty());
    CHECK(session.outline(destinationDate).value().entries ==
          destination.entries);

    session.close();
    REQUIRE(session.open(temporaryDirectory.path() / "cross-page-move.hieda"));
    CHECK(session.outline(namedId).value().entries.empty());
    CHECK(session.outline(destinationDate).value().entries ==
          destination.entries);

    const auto archive = session.createPage("archive", "Archive").value();
    REQUIRE(session.moveEntryToPage(parent.metadata.id, archive.metadata.id,
                                    std::nullopt));
    CHECK(session.outline(destinationDate).value().entries.empty());
    const auto archived = session.outline(archive.metadata.id).value();
    REQUIRE(archived.entries.size() == 2);
    CHECK(archived.entries[0].metadata.id == parent.metadata.id);
    CHECK(archived.entries[0].metadata.createdAt == parent.metadata.createdAt);
    CHECK(archived.entries[1].metadata.id == child.metadata.id);
    CHECK(archived.entries[1].parentEntry == parent.metadata.id);
}

TEST_CASE("a saved Query selects Entry Blocks and survives reopening")
{
    TemporaryDirectory temporaryDirectory;
    const auto notebookPath = temporaryDirectory.path() / "queries.hieda";
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(notebookPath));
    const auto page = session.createPage("queries", "Queries").value();
    REQUIRE(session.insertEntry(page.metadata.id, std::nullopt, "ordinary"));
    const std::string querySource = "{{query (where (type entry))}}";
    const auto inserted =
        session.insertEntry(page.metadata.id, std::nullopt, querySource)
            .value();
    const auto queryId = inserted.entries.back().metadata.id;

    const auto evaluated = session.evaluateQuery(queryId);

    REQUIRE(evaluated);
    CHECK(evaluated.value().hasQueryIntent);
    CHECK_FALSE(evaluated.value().error);
    REQUIRE(evaluated.value().rows.size() == 2);
    CHECK(std::ranges::all_of(
        evaluated.value().rows, [](const auto& row) -> bool {
            return row.type == hieda::notebook::QueryResultBlockType::entry;
        }));
    CHECK_FALSE(evaluated.value().continuationCursor);

    session.close();
    REQUIRE(session.open(notebookPath));
    const auto reopened = session.evaluateQuery(queryId);
    REQUIRE(reopened);
    CHECK(reopened.value().rows == evaluated.value().rows);
}

TEST_CASE("Queries distinguish Block type from containing Page context")
{
    TemporaryDirectory temporaryDirectory;
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(temporaryDirectory.path() / "query-context.hieda"));
    const auto named = session.createPage("work", "Work").value();
    REQUIRE(session.insertEntry(named.metadata.id, std::nullopt, "named"));
    const auto query = session
                           .insertEntry(named.metadata.id, std::nullopt,
                                        "{{query (where (type page))}}")
                           .value()
                           .entries.back();
    const hieda::notebook::JournalDate date{2026, 8, 10};
    const auto journal =
        session.insertEntry(date, std::nullopt, "journal").value();

    auto evaluated = session.evaluateQuery(query.metadata.id);
    REQUIRE(evaluated);
    REQUIRE(evaluated.value().rows.size() == 2);
    CHECK(std::ranges::all_of(
        evaluated.value().rows, [](const auto& row) -> bool {
            return row.type == hieda::notebook::QueryResultBlockType::page;
        }));

    REQUIRE(session.updateEntry(query.metadata.id,
                                "{{query (where (page-context named))}}"));
    evaluated = session.evaluateQuery(query.metadata.id);
    REQUIRE(evaluated);
    REQUIRE(evaluated.value().rows.size() == 3);
    CHECK(std::ranges::all_of(
        evaluated.value().rows, [](const auto& row) -> bool {
            return row.pageKind == hieda::notebook::PageKind::named;
        }));

    REQUIRE(session.updateEntry(query.metadata.id,
                                "{{query (where (page-context journal))}}"));
    evaluated = session.evaluateQuery(query.metadata.id);
    REQUIRE(evaluated);
    REQUIRE(evaluated.value().rows.size() == 2);
    CHECK(std::ranges::all_of(
        evaluated.value().rows, [&](const auto& row) -> bool {
            return row.pageKind == hieda::notebook::PageKind::journal &&
                   row.journalDate == date;
        }));
    CHECK(std::ranges::any_of(
        evaluated.value().rows, [&](const auto& row) -> bool {
            return row.metadata.id == journal.metadata.value().id;
        }));
}

TEST_CASE("Journal Date Query predicates apply to the complete Page Context")
{
    TemporaryDirectory temporaryDirectory;
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(temporaryDirectory.path() / "query-dates.hieda"));
    const auto named = session.createPage("queries", "Queries").value();
    const auto query =
        session
            .insertEntry(named.metadata.id, std::nullopt,
                         "{{query (where (journal-date = 2026-08-10))}}")
            .value()
            .entries.back();
    for (const auto date : {hieda::notebook::JournalDate{2026, 8, 9},
                            hieda::notebook::JournalDate{2026, 8, 10},
                            hieda::notebook::JournalDate{2026, 8, 11}}) {
        REQUIRE(session.insertEntry(date, std::nullopt, "dated"));
    }
    const auto evaluate = [&](const std::string& comparison)
        -> std::vector<hieda::notebook::QueryResultRow> {
        REQUIRE(session.updateEntry(query.metadata.id,
                                    "{{query (where (journal-date " +
                                        comparison + " 2026-08-10))}}"));
        const auto result = session.evaluateQuery(query.metadata.id);
        REQUIRE(result);
        CHECK_FALSE(result.value().error);
        return result.value().rows;
    };

    const auto equal = evaluate("=");
    REQUIRE(equal.size() == 2);
    CHECK(std::ranges::all_of(equal, [](const auto& row) -> bool {
        return row.journalDate == hieda::notebook::JournalDate{2026, 8, 10};
    }));
    CHECK(evaluate("<").size() == 2);
    CHECK(evaluate("<=").size() == 4);
    CHECK(evaluate(">").size() == 2);
    CHECK(evaluate(">=").size() == 4);
}

TEST_CASE("text-contains Queries use literal exact Authored Text")
{
    TemporaryDirectory temporaryDirectory;
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(temporaryDirectory.path() / "query-text.hieda"));
    const auto page = session.createPage("queries", "Queries").value();
    const auto first = session
                           .insertEntry(page.metadata.id, std::nullopt,
                                        "status::open\nCafé planning")
                           .value()
                           .entries.back();
    REQUIRE(session.insertEntry(page.metadata.id, std::nullopt,
                                "status::closed\ncafé planning"));
    const auto query =
        session
            .insertEntry(page.metadata.id, std::nullopt,
                         "{{query (where (text-contains \"Café\"))}}")
            .value()
            .entries.back();

    const auto evaluated = session.evaluateQuery(query.metadata.id);

    REQUIRE(evaluated);
    CHECK_FALSE(evaluated.value().error);
    REQUIRE(evaluated.value().rows.size() == 2);
    CHECK(std::ranges::any_of(evaluated.value().rows,
                              [&](const auto& row) -> bool {
                                  return row.metadata.id == first.metadata.id;
                              }));
    CHECK(std::ranges::any_of(evaluated.value().rows,
                              [&](const auto& row) -> bool {
                                  return row.metadata.id == query.metadata.id;
                              }));

    REQUIRE(session.updateEntry(
        query.metadata.id,
        "{{query (where (text-contains \"status::open\"))}}"));
    CHECK(session.evaluateQuery(query.metadata.id).value().rows.size() == 2);

    REQUIRE(session.updateEntry(query.metadata.id,
                                "{{query (where (text-contains \"\"))}}"));
    const auto invalid = session.evaluateQuery(query.metadata.id);
    REQUIRE(invalid);
    CHECK(invalid.value().error);
    CHECK(invalid.value().rows.empty());
}

TEST_CASE("Property Queries preserve duplicate exact authored values")
{
    TemporaryDirectory temporaryDirectory;
    hieda::notebook::NotebookSession session;
    REQUIRE(
        session.create(temporaryDirectory.path() / "query-properties.hieda"));
    const auto page = session.createPage("queries", "Queries").value();
    const auto duplicate =
        session
            .insertEntry(page.metadata.id, std::nullopt,
                         "status::open\nstatus::closed\nowner::")
            .value()
            .entries.back();
    REQUIRE(session.insertEntry(page.metadata.id, std::nullopt,
                                "status::Open\n\\owner::escaped"));
    const auto query =
        session
            .insertEntry(page.metadata.id, std::nullopt,
                         "{{query (where (property-exists status))}}")
            .value()
            .entries.back();

    auto evaluated = session.evaluateQuery(query.metadata.id);
    REQUIRE(evaluated);
    CHECK_FALSE(evaluated.value().error);
    CHECK(evaluated.value().rows.size() == 2);

    REQUIRE(session.updateEntry(
        query.metadata.id,
        "{{query (where (property-equals status \"closed\"))}}"));
    evaluated = session.evaluateQuery(query.metadata.id);
    REQUIRE(evaluated);
    REQUIRE(evaluated.value().rows.size() == 1);
    CHECK(evaluated.value().rows.front().metadata.id == duplicate.metadata.id);

    REQUIRE(session.updateEntry(
        query.metadata.id, "{{query (where (property-equals owner \"\"))}}"));
    evaluated = session.evaluateQuery(query.metadata.id);
    REQUIRE(evaluated);
    REQUIRE(evaluated.value().rows.size() == 1);
    CHECK(evaluated.value().rows.front().metadata.id == duplicate.metadata.id);
}

TEST_CASE("Query predicates compose with strict and or and not rules")
{
    TemporaryDirectory temporaryDirectory;
    hieda::notebook::NotebookSession session;
    REQUIRE(
        session.create(temporaryDirectory.path() / "query-composition.hieda"));
    const auto page = session.createPage("queries", "Queries").value();
    const auto open =
        session.insertEntry(page.metadata.id, std::nullopt, "status::open")
            .value()
            .entries.back();
    REQUIRE(
        session.insertEntry(page.metadata.id, std::nullopt, "status::closed"));
    const auto query = session
                           .insertEntry(page.metadata.id, std::nullopt,
                                        "{{query (where (and (type entry) "
                                        "(property-equals status \"open\")))}}")
                           .value()
                           .entries.back();

    auto evaluated = session.evaluateQuery(query.metadata.id);
    REQUIRE(evaluated);
    REQUIRE(evaluated.value().rows.size() == 1);
    CHECK(evaluated.value().rows.front().metadata.id == open.metadata.id);

    REQUIRE(session.updateEntry(query.metadata.id,
                                "{{query (where (or (type page) "
                                "(property-equals status \"closed\")))}}"));
    evaluated = session.evaluateQuery(query.metadata.id);
    REQUIRE(evaluated);
    CHECK(evaluated.value().rows.size() == 2);

    REQUIRE(session.updateEntry(query.metadata.id,
                                "{{query (where (and (type entry) "
                                "(not (property-exists status))))}}"));
    evaluated = session.evaluateQuery(query.metadata.id);
    REQUIRE(evaluated);
    REQUIRE(evaluated.value().rows.size() == 1);
    CHECK(evaluated.value().rows.front().metadata.id == query.metadata.id);

    REQUIRE(session.updateEntry(query.metadata.id, "{{query (where (all))}}"));
    CHECK(session.evaluateQuery(query.metadata.id).value().rows.size() == 4);

    for (const auto* invalid :
         {"{{query (where (and (all)))}}", "{{query (where (or (all)))}}",
          "{{query (where (not (all) (all)))}}"}) {
        REQUIRE(session.updateEntry(query.metadata.id, invalid));
        const auto result = session.evaluateQuery(query.metadata.id);
        REQUIRE(result);
        CHECK(result.value().error);
        CHECK(result.value().rows.empty());
    }
}

TEST_CASE("Query ordering limits and revision-bound batches are deterministic")
{
    TemporaryDirectory temporaryDirectory;
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(temporaryDirectory.path() / "query-order.hieda"));
    const auto page = session.createPage("queries", "Queries").value();
    std::vector<hieda::notebook::BlockId> createdIds;
    for (const auto* text : {"order::first", "order::second", "order::third"}) {
        createdIds.push_back(
            session.insertEntry(page.metadata.id, std::nullopt, text)
                .value()
                .entries.back()
                .metadata.id);
    }
    const auto query =
        session
            .insertEntry(page.metadata.id, std::nullopt,
                         "{{query (where (property-exists order)) "
                         "(order-by creation-time asc)}}")
            .value()
            .entries.back();
    const auto resultIds =
        [&](const auto& result) -> std::vector<hieda::notebook::BlockId> {
        std::vector<hieda::notebook::BlockId> ids;
        for (const auto& row : result.value().rows) {
            ids.push_back(row.metadata.id);
        }
        return ids;
    };

    auto evaluated = session.evaluateQuery(query.metadata.id);
    REQUIRE(evaluated);
    CHECK(resultIds(evaluated) == createdIds);

    REQUIRE(session.updateEntry(query.metadata.id,
                                "{{query (where (property-exists order)) "
                                "(order-by creation-time desc)}}"));
    evaluated = session.evaluateQuery(query.metadata.id);
    REQUIRE(evaluated);
    auto reverseIds = createdIds;
    std::ranges::reverse(reverseIds);
    CHECK(resultIds(evaluated) == reverseIds);

    REQUIRE(session.updateEntry(createdIds.front(), "order::first updated"));
    REQUIRE(session.updateEntry(query.metadata.id,
                                "{{query (where (property-exists order)) "
                                "(order-by update-time desc) (limit 2)}}"));
    evaluated = session.evaluateQuery(query.metadata.id);
    REQUIRE(evaluated);
    REQUIRE(evaluated.value().rows.size() == 2);
    CHECK(evaluated.value().rows.front().metadata.id == createdIds.front());
    CHECK_FALSE(evaluated.value().continuationCursor);

    REQUIRE(session.updateEntry(query.metadata.id,
                                "{{query (where (property-exists order)) "
                                "(order-by update-time asc)}}"));
    evaluated = session.evaluateQuery(query.metadata.id);
    REQUIRE(evaluated);
    CHECK(resultIds(evaluated) ==
          std::vector<hieda::notebook::BlockId>{createdIds[1], createdIds[2],
                                                createdIds[0]});

    REQUIRE(session.updateEntry(query.metadata.id,
                                "{{query (where (property-exists order))}}"));
    const auto defaultOrder = session.evaluateQuery(query.metadata.id);
    REQUIRE(defaultOrder);
    REQUIRE(defaultOrder.value().rows.size() == 3);
    CHECK(defaultOrder.value().rows.front().metadata.id == createdIds.front());
    const auto repeatedDefaultOrder = session.evaluateQuery(query.metadata.id);
    REQUIRE(repeatedDefaultOrder);
    CHECK(resultIds(defaultOrder) == resultIds(repeatedDefaultOrder));

    REQUIRE(session.updateEntry(query.metadata.id,
                                "{{query (where (type entry))}}"));
    for (int index = 0; index < 100; ++index) {
        REQUIRE(session.insertEntry(page.metadata.id, std::nullopt,
                                    "bulk " + std::to_string(index)));
    }
    const auto firstBatch = session.evaluateQuery(query.metadata.id);
    REQUIRE(firstBatch);
    CHECK(firstBatch.value().rows.size() == 100);
    REQUIRE(firstBatch.value().continuationCursor);
    const auto secondBatch = session.evaluateQuery(
        query.metadata.id, firstBatch.value().continuationCursor);
    REQUIRE(secondBatch);
    CHECK(secondBatch.value().rows.size() == 4);
    CHECK_FALSE(secondBatch.value().continuationCursor);

    const auto staleCursor = firstBatch.value().continuationCursor;
    REQUIRE(session.updateEntry(createdIds.back(), "order::changed"));
    const auto stale = session.evaluateQuery(query.metadata.id, staleCursor);
    REQUIRE_FALSE(stale);
    CHECK(stale.error().code ==
          hieda::notebook::NotebookErrorCode::staleQueryCursor);
}

TEST_CASE("Query ordering ties and Named Journal ordering use stable identity")
{
    TemporaryDirectory temporaryDirectory;
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(temporaryDirectory.path() / "query-ties.hieda"));
    const auto page = session.createPage("queries", "Queries").value();
    std::vector<hieda::notebook::BlockId> tiedIds;
    {
        const TimestampOverride timestamp(
            hieda::notebook::BlockTimestamp{std::chrono::microseconds{1000}});
        for (const auto* text : {"tie::one", "tie::two", "tie::three"}) {
            tiedIds.push_back(
                session.insertEntry(page.metadata.id, std::nullopt, text)
                    .value()
                    .entries.back()
                    .metadata.id);
        }
    }
    auto stableIds = tiedIds;
    std::ranges::sort(stableIds,
                      [](const auto& left, const auto& right) -> bool {
                          return std::ranges::lexicographical_compare(
                              left.bytes, right.bytes);
                      });
    const auto query = session
                           .insertEntry(page.metadata.id, std::nullopt,
                                        "{{query (where (property-exists tie)) "
                                        "(order-by creation-time desc)}}")
                           .value()
                           .entries.back();
    const auto resultIds =
        [](const auto& result) -> std::vector<hieda::notebook::BlockId> {
        std::vector<hieda::notebook::BlockId> ids;
        for (const auto& row : result.value().rows) {
            ids.push_back(row.metadata.id);
        }
        return ids;
    };

    auto evaluated = session.evaluateQuery(query.metadata.id);
    REQUIRE(evaluated);
    CHECK(resultIds(evaluated) == stableIds);
    REQUIRE(session.updateEntry(query.metadata.id,
                                "{{query (where (property-exists tie)) "
                                "(order-by creation-time asc)}}"));
    evaluated = session.evaluateQuery(query.metadata.id);
    REQUIRE(evaluated);
    CHECK(resultIds(evaluated) == stableIds);

    {
        const TimestampOverride timestamp(
            hieda::notebook::BlockTimestamp{std::chrono::microseconds{2000}});
        for (std::size_t index = 0; index < tiedIds.size(); ++index) {
            REQUIRE(session.updateEntry(
                tiedIds[index], "tie::updated " + std::to_string(index)));
        }
    }
    for (const auto* direction : {"asc", "desc"}) {
        REQUIRE(session.updateEntry(
            query.metadata.id,
            "{{query (where (property-exists tie)) (order-by update-time " +
                std::string(direction) + ")}}"));
        evaluated = session.evaluateQuery(query.metadata.id);
        REQUIRE(evaluated);
        CHECK(resultIds(evaluated) == stableIds);
    }

    const auto archive = session.createPage("archive", "Archive").value();
    const auto inbox = session.createPage("inbox", "Inbox").value();
    auto namedPageIds =
        std::vector{page.metadata.id, archive.metadata.id, inbox.metadata.id};
    std::ranges::sort(namedPageIds,
                      [](const auto& left, const auto& right) -> bool {
                          return std::ranges::lexicographical_compare(
                              left.bytes, right.bytes);
                      });
    REQUIRE(session.updateEntry(
        query.metadata.id,
        "{{query (where (and (type page) (page-context named))) "
        "(order-by journal-date desc)}}"));
    evaluated = session.evaluateQuery(query.metadata.id);
    REQUIRE(evaluated);
    CHECK(resultIds(evaluated) == namedPageIds);
}

TEST_CASE("Journal Date ordering keeps Page roots and outline order together")
{
    TemporaryDirectory temporaryDirectory;
    hieda::notebook::NotebookSession session;
    REQUIRE(
        session.create(temporaryDirectory.path() / "query-date-order.hieda"));
    const auto named = session.createPage("queries", "Queries").value();
    const auto query = session
                           .insertEntry(named.metadata.id, std::nullopt,
                                        "{{query (where (all)) "
                                        "(order-by journal-date desc)}}")
                           .value()
                           .entries.back();
    const hieda::notebook::JournalDate earlier{2026, 8, 10};
    const hieda::notebook::JournalDate later{2026, 8, 11};
    const auto earlierPage =
        session.insertEntry(earlier, std::nullopt, "earlier").value();
    auto laterPage = session.insertEntry(later, std::nullopt, "parent").value();
    const auto parent = laterPage.entries.front();
    laterPage = session.insertEntry(later, parent.metadata.id, "child").value();
    const auto child = laterPage.entries.back();
    laterPage =
        session
            .moveEntry(child.metadata.id, hieda::notebook::EntryMove::indent,
                       child.authoredText)
            .value();
    REQUIRE(session.updateEntry(earlierPage.entries.front().metadata.id,
                                "earlier updated last"));

    auto evaluated = session.evaluateQuery(query.metadata.id);
    REQUIRE(evaluated);
    REQUIRE(evaluated.value().rows.size() == 7);
    CHECK(evaluated.value().rows[0].journalDate == later);
    CHECK(evaluated.value().rows[0].type ==
          hieda::notebook::QueryResultBlockType::page);
    CHECK(evaluated.value().rows[1].metadata.id == parent.metadata.id);
    CHECK(evaluated.value().rows[2].metadata.id == child.metadata.id);
    CHECK(evaluated.value().rows[3].journalDate == earlier);
    CHECK(evaluated.value().rows[3].type ==
          hieda::notebook::QueryResultBlockType::page);
    CHECK(evaluated.value().rows[4].metadata.id ==
          earlierPage.entries.front().metadata.id);
    CHECK(evaluated.value().rows[5].pageKind ==
          hieda::notebook::PageKind::named);
    CHECK(evaluated.value().rows[6].pageKind ==
          hieda::notebook::PageKind::named);

    REQUIRE(session.updateEntry(query.metadata.id,
                                "{{query (where (page-context journal)) "
                                "(order-by journal-date asc)}}"));
    evaluated = session.evaluateQuery(query.metadata.id);
    REQUIRE(evaluated);
    REQUIRE(evaluated.value().rows.size() == 5);
    CHECK(evaluated.value().rows[0].journalDate == earlier);
    CHECK(evaluated.value().rows[0].type ==
          hieda::notebook::QueryResultBlockType::page);
    CHECK(evaluated.value().rows[2].journalDate == later);
    CHECK(evaluated.value().rows[2].type ==
          hieda::notebook::QueryResultBlockType::page);
    CHECK(evaluated.value().rows[3].metadata.id == parent.metadata.id);
    CHECK(evaluated.value().rows[4].metadata.id == child.metadata.id);
}

TEST_CASE("invalid Query intent is editable diagnosed and never partially run")
{
    TemporaryDirectory temporaryDirectory;
    const auto notebookPath = temporaryDirectory.path() / "query-errors.hieda";
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(notebookPath));
    const auto target = session.createPage("target", "Target").value();
    const auto page = session.createPage("queries", "Queries").value();
    REQUIRE(session.insertEntry(page.metadata.id, std::nullopt, "ordinary"));
    const auto query = session
                           .insertEntry(page.metadata.id, std::nullopt,
                                        "{{query (where (type entry))}}")
                           .value()
                           .entries.back();
    REQUIRE(session.evaluateQuery(query.metadata.id).value().rows.size() == 2);

    const std::vector<std::string> invalidSources{
        "{{query",
        "{{query (where (type entry)) (where (all))}}",
        "{{query (where (unknown value))}}",
        R"({{query (where (text-contains "bad\t"))}})",
        "{{query (where (text-contains \"raw\nnewline\"))}}",
        "{{query (where (journal-date = 2026-02-30))}}",
        "{{query (where (all)) (limit 0)}}",
        "{{query (where (all)) (limit 18446744073709551616)}}",
        "{{query (where (all)) (limit 1) (order-by update-time asc)}}",
        "{{query (where (all)) extra)}}",
        "{{query (where self)}}",
        "{{query (where (child-of [[Bad Name]]))}}",
        "{{query (where (page-links-to))}}",
        "{{query (where (in-page-subtree self))}}",
        "{{query (where (in-page-subtree "
        "[[block:550e8400-e29b-41d4-a716-446655440000]]))}}",
        "{{query (where [[block:not-a-uuid]])}}",
    };
    for (const auto& source : invalidSources) {
        REQUIRE(session.updateEntry(query.metadata.id, source));
        const auto result = session.evaluateQuery(query.metadata.id);
        REQUIRE(result);
        CHECK(result.value().hasQueryIntent);
        const auto* error =
            result.value().error ? &result.value().error.value() : nullptr;
        REQUIRE(error != nullptr);
        CHECK_FALSE(error->message.empty());
        CHECK(error->sourceByteOffset <= source.size());
        CHECK(result.value().rows.empty());
    }

    const std::string opaque =
        "{{query (where (unknown [[target]] "
        "[[block:550e8400-e29b-41d4-a716-446655440000]]))}}";
    REQUIRE(session.updateEntry(query.metadata.id, opaque));
    CHECK(session.pageLinks(query.metadata.id).value().empty());
    CHECK(session.blockReferences(query.metadata.id).value().empty());
    CHECK(session.linkedReferences(target.metadata.id).value().sources.empty());

    session.close();
    REQUIRE(session.open(notebookPath));
    const auto reopened = session.evaluateQuery(query.metadata.id);
    REQUIRE(reopened);
    CHECK(reopened.value().error);
    CHECK(
        session.outline(page.metadata.id).value().entries.back().authoredText ==
        opaque);

    REQUIRE(session.updateEntry(query.metadata.id, "ordinary {{query text"));
    const auto ordinary = session.evaluateQuery(query.metadata.id);
    REQUIRE(ordinary);
    CHECK_FALSE(ordinary.value().hasQueryIntent);
    CHECK_FALSE(ordinary.value().error);
    CHECK(ordinary.value().rows.empty());
}

TEST_CASE("Query results reflect committed edits moves undo and redo")
{
    TemporaryDirectory temporaryDirectory;
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(temporaryDirectory.path() / "query-live.hieda"));
    const auto source = session.createPage("source", "Source").value();
    const auto destination =
        session.createPage("destination", "Destination").value();
    const auto candidate =
        session.insertEntry(source.metadata.id, std::nullopt, "status::closed")
            .value()
            .entries.back();
    const auto query =
        session
            .insertEntry(source.metadata.id, std::nullopt,
                         "{{query (where (property-equals status \"open\"))}}")
            .value()
            .entries.back();

    CHECK(session.evaluateQuery(query.metadata.id).value().rows.empty());
    REQUIRE(session.updateEntry(candidate.metadata.id, "status::open"));
    REQUIRE(session.evaluateQuery(query.metadata.id).value().rows.size() == 1);
    CHECK(session.evaluateQuery(query.metadata.id)
              .value()
              .rows.front()
              .metadata.id == candidate.metadata.id);

    REQUIRE(session.updateEntry(query.metadata.id,
                                "{{query (where (page-context named))}}"));
    const auto beforeMove = session.evaluateQuery(query.metadata.id).value();
    REQUIRE(session.moveEntryToPage(candidate.metadata.id,
                                    destination.metadata.id, std::nullopt));
    const auto afterMove = session.evaluateQuery(query.metadata.id).value();
    REQUIRE(afterMove.rows.size() == beforeMove.rows.size());
    const auto moved =
        std::ranges::find_if(afterMove.rows, [&](const auto& row) -> bool {
            return row.metadata.id == candidate.metadata.id;
        });
    REQUIRE(moved != afterMove.rows.end());
    CHECK(moved->contextPageId == destination.metadata.id);

    REQUIRE(session.undoEdit());
    const auto undone = session.evaluateQuery(query.metadata.id).value();
    const auto restored =
        std::ranges::find_if(undone.rows, [&](const auto& row) -> bool {
            return row.metadata.id == candidate.metadata.id;
        });
    REQUIRE(restored != undone.rows.end());
    CHECK(restored->contextPageId == source.metadata.id);
    REQUIRE(session.redoEdit());
    const auto redone = session.evaluateQuery(query.metadata.id).value();
    CHECK(std::ranges::any_of(redone.rows, [&](const auto& row) -> bool {
        return row.metadata.id == candidate.metadata.id &&
               row.contextPageId == destination.metadata.id;
    }));
}
TEST_CASE("Containment Query predicates distinguish direct and transitive "
          "relationships")
{
    TemporaryDirectory temporaryDirectory;
    hieda::notebook::NotebookSession session;
    const auto notebookPath =
        temporaryDirectory.path() / "query-containment.hieda";
    REQUIRE(session.create(notebookPath));
    const auto page = session.createPage("outline", "Outline").value();
    auto outline =
        session.insertEntry(page.metadata.id, std::nullopt, "parent").value();
    const auto parent = outline.entries.back();
    outline = session.insertEntry(page.metadata.id, parent.metadata.id, "child")
                  .value();
    const auto child = outline.entries.back();
    REQUIRE(session.moveEntry(child.metadata.id,
                              hieda::notebook::EntryMove::indent, "child"));
    outline =
        session.insertEntry(page.metadata.id, child.metadata.id, "grandchild")
            .value();
    const auto grandchild = outline.entries.back();
    REQUIRE(session.moveEntry(grandchild.metadata.id,
                              hieda::notebook::EntryMove::indent,
                              "grandchild"));
    const auto queryPage = session.createPage("queries", "Queries").value();
    const auto query =
        session
            .insertEntry(queryPage.metadata.id, std::nullopt,
                         "{{query (where (child-of [[block:" +
                             parent.metadata.id.toString() + "]])))}}")
            .value()
            .entries.back();

    const auto matches = [&](std::string predicate) {
        REQUIRE(session.updateEntry(query.metadata.id,
                                    "{{query (where " + predicate + ")}}"));
        const auto evaluated = session.evaluateQuery(query.metadata.id);
        REQUIRE(evaluated);
        REQUIRE_FALSE(evaluated.value().error);
        std::vector<hieda::notebook::BlockId> identifiers;
        for (const auto& row : evaluated.value().rows) {
            identifiers.push_back(row.metadata.id);
        }
        std::ranges::sort(identifiers, {}, [](const auto& identifier) {
            return identifier.toString();
        });
        return identifiers;
    };
    const auto sorted = [](std::vector<hieda::notebook::BlockId> identifiers) {
        std::ranges::sort(identifiers, {}, [](const auto& identifier) {
            return identifier.toString();
        });
        return identifiers;
    };
    const auto anchor = [](hieda::notebook::BlockId id) {
        return "[[block:" + id.toString() + "]]";
    };

    CHECK(matches("(child-of " + anchor(parent.metadata.id) + ")") ==
          sorted({child.metadata.id}));
    CHECK(matches("(descendant-of " + anchor(parent.metadata.id) + ")") ==
          sorted({child.metadata.id, grandchild.metadata.id}));
    CHECK(matches("(parent-of " + anchor(child.metadata.id) + ")") ==
          sorted({parent.metadata.id}));
    CHECK(matches("(ancestor-of " + anchor(grandchild.metadata.id) + ")") ==
          sorted({page.metadata.id, parent.metadata.id, child.metadata.id}));
    CHECK(matches("(parent-of self)") == sorted({queryPage.metadata.id}));

    REQUIRE(session.moveEntry(grandchild.metadata.id,
                              hieda::notebook::EntryMove::outdent,
                              "grandchild"));
    CHECK(matches("(child-of " + anchor(child.metadata.id) + ")").empty());

    session.close();
    REQUIRE(session.open(notebookPath));
    CHECK(matches("(ancestor-of " + anchor(grandchild.metadata.id) + ")") ==
          sorted({page.metadata.id, parent.metadata.id}));
}

TEST_CASE("in-page-subtree Queries use inclusive slash-bounded Page Hierarchy")
{
    TemporaryDirectory temporaryDirectory;
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(temporaryDirectory.path() /
                           "query-page-hierarchy.hieda"));
    const auto projects = session.createPage("projects", "Projects").value();
    const auto hieda = session.createPage("projects/hieda", "Hieda").value();
    const auto specification =
        session.createPage("projects/hieda/spec", "Spec").value();
    const auto similar =
        session.createPage("projectiles", "Projectiles").value();
    const auto entry =
        session.insertEntry(hieda.metadata.id, std::nullopt, "work")
            .value()
            .entries.back();
    const auto queryPage = session.createPage("queries", "Queries").value();
    const auto query =
        session
            .insertEntry(queryPage.metadata.id, std::nullopt,
                         "{{query (where (in-page-subtree [[projects]]))}}")
            .value()
            .entries.back();

    const auto evaluated = session.evaluateQuery(query.metadata.id);
    REQUIRE(evaluated);
    REQUIRE_FALSE(evaluated.value().error);
    const auto contains = [&](hieda::notebook::BlockId identifier) {
        return std::ranges::any_of(
            evaluated.value().rows,
            [&](const auto& row) { return row.metadata.id == identifier; });
    };
    CHECK(contains(projects.metadata.id));
    CHECK(contains(hieda.metadata.id));
    CHECK(contains(specification.metadata.id));
    CHECK(contains(entry.metadata.id));
    CHECK_FALSE(contains(similar.metadata.id));
    CHECK(evaluated.value().rows.size() == 4);

    const auto future =
        session.createPage("future/child", "Future Child").value();
    REQUIRE(session.updateEntry(
        query.metadata.id, "{{query (where (in-page-subtree [[future]]))}}"));
    const auto previewRoot = session.evaluateQuery(query.metadata.id);
    REQUIRE(previewRoot);
    REQUIRE(previewRoot.value().rows.size() == 1);
    CHECK(previewRoot.value().rows.front().metadata.id == future.metadata.id);
}

TEST_CASE("Semantic Reference Queries distinguish literal outgoing and "
          "incoming relationships")
{
    TemporaryDirectory temporaryDirectory;
    const auto notebookPath =
        temporaryDirectory.path() / "query-semantic-references.hieda";
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(notebookPath));
    const auto targetPage = session.createPage("target", "Target").value();
    const auto contentPage = session.createPage("content", "Content").value();
    const auto targetEntry =
        session
            .insertEntry(contentPage.metadata.id, std::nullopt, "target entry")
            .value()
            .entries.back();
    const auto sourceText =
        "[[target]] [[block:" + targetEntry.metadata.id.toString() + "]]";
    const auto source = session
                            .insertEntry(contentPage.metadata.id,
                                         targetEntry.metadata.id, sourceText)
                            .value()
                            .entries.back();
    REQUIRE(session.updateEntry(
        targetEntry.metadata.id,
        "cycle [[block:" + source.metadata.id.toString() + "]]"));
    const auto queryPage = session.createPage("queries", "Queries").value();
    const auto query = session
                           .insertEntry(queryPage.metadata.id, std::nullopt,
                                        "{{query (where [[target]])}}")
                           .value()
                           .entries.back();

    const auto matches = [&](std::string predicate) {
        REQUIRE(session.updateEntry(query.metadata.id,
                                    "{{query (where " + predicate + ")}}"));
        const auto result = session.evaluateQuery(query.metadata.id);
        REQUIRE(result);
        REQUIRE_FALSE(result.value().error);
        std::vector<hieda::notebook::BlockId> identifiers;
        for (const auto& row : result.value().rows) {
            identifiers.push_back(row.metadata.id);
        }
        return identifiers;
    };
    const auto anchor = [](hieda::notebook::BlockId identifier) {
        return "[[block:" + identifier.toString() + "]]";
    };

    CHECK(matches("[[target]]") == std::vector{source.metadata.id});
    CHECK(matches(anchor(targetEntry.metadata.id)) ==
          std::vector{source.metadata.id});
    CHECK(matches("(and (page-links-to [[target]]) (block-references " +
                  anchor(targetEntry.metadata.id) + "))") ==
          std::vector{source.metadata.id});
    CHECK(matches("(page-links-to " + anchor(targetPage.metadata.id) + ")") ==
          std::vector{source.metadata.id});
    CHECK(matches("(linked-by " + anchor(source.metadata.id) + ")") ==
          std::vector{targetPage.metadata.id});
    CHECK(matches("(block-referenced-by " + anchor(source.metadata.id) + ")") ==
          std::vector{targetEntry.metadata.id});
    CHECK(matches("(block-referenced-by " + anchor(targetEntry.metadata.id) +
                  ")") == std::vector{source.metadata.id});

    CHECK(matches("(and (page-links-to [[target]]) (block-references " +
                  anchor(targetEntry.metadata.id) + "))") ==
          std::vector{source.metadata.id});
    REQUIRE(session.updateEntry(source.metadata.id, "references removed"));
    CHECK(session.evaluateQuery(query.metadata.id).value().rows.empty());
    REQUIRE(session.updateEntry(source.metadata.id, sourceText));
    const auto restoredReferences = session.evaluateQuery(query.metadata.id);
    REQUIRE(restoredReferences);
    REQUIRE(restoredReferences.value().rows.size() == 1);
    CHECK(restoredReferences.value().rows.front().metadata.id ==
          source.metadata.id);

    REQUIRE(session.updateEntry(
        query.metadata.id, "{{query (where (page-links-to [[target]]))}}"));
    const auto queryUpdatedAt =
        session.locateBlock(query.metadata.id).value().target.updatedAt;
    REQUIRE(session.renamePage(targetPage.metadata.id, "renamed", "Renamed"));
    const auto renamed = session.evaluateQuery(query.metadata.id);
    REQUIRE(renamed);
    REQUIRE_FALSE(renamed.value().error);
    REQUIRE(renamed.value().rows.size() == 1);
    CHECK(renamed.value().rows.front().metadata.id == source.metadata.id);
    const auto queryAfterRename = session.locateBlock(query.metadata.id);
    REQUIRE(queryAfterRename);
    CHECK(queryAfterRename.value().target.updatedAt == queryUpdatedAt);
    const auto queryOutline = session.outline(queryPage.metadata.id).value();
    const auto savedQuery =
        std::ranges::find(queryOutline.entries, query.metadata.id,
                          [](const auto& entry) { return entry.metadata.id; });
    REQUIRE(savedQuery != queryOutline.entries.end());
    CHECK(savedQuery->authoredText ==
          "{{query (where (page-links-to [[renamed]]))}}");

    REQUIRE(session.deletePage(targetPage.metadata.id));
    CHECK(session.evaluateQuery(query.metadata.id).value().rows.empty());
    REQUIRE(session.undoEdit());
    const auto restoredPageLink = session.evaluateQuery(query.metadata.id);
    REQUIRE(restoredPageLink);
    REQUIRE(restoredPageLink.value().rows.size() == 1);
    CHECK(restoredPageLink.value().rows.front().metadata.id ==
          source.metadata.id);

    REQUIRE(session.deletePage(targetPage.metadata.id));
    REQUIRE(session.updateEntry(query.metadata.id,
                                "{{query (where [[renamed]])}}"));
    const auto unresolvedLiteral = session.evaluateQuery(query.metadata.id);
    REQUIRE(unresolvedLiteral);
    REQUIRE(unresolvedLiteral.value().rows.size() == 1);
    CHECK(unresolvedLiteral.value().rows.front().metadata.id ==
          source.metadata.id);
    REQUIRE(session.undoEdit());
    REQUIRE(session.undoEdit());

    CHECK(matches("(block-references " + anchor(targetEntry.metadata.id) +
                  ")") == std::vector{source.metadata.id});
    REQUIRE(session.deleteEntry(targetEntry.metadata.id));
    CHECK(session.evaluateQuery(query.metadata.id).value().rows.empty());
    REQUIRE(session.undoEdit());
    const auto restoredBlock = session.evaluateQuery(query.metadata.id);
    REQUIRE(restoredBlock);
    REQUIRE(restoredBlock.value().rows.size() == 1);
    CHECK(restoredBlock.value().rows.front().metadata.id == source.metadata.id);

    REQUIRE(session.deleteEntry(targetEntry.metadata.id));
    REQUIRE(session.updateEntry(query.metadata.id,
                                "{{query (where " +
                                    anchor(targetEntry.metadata.id) + ")}}"));
    const auto missingLiteral = session.evaluateQuery(query.metadata.id);
    REQUIRE(missingLiteral);
    REQUIRE(missingLiteral.value().rows.size() == 1);
    CHECK(missingLiteral.value().rows.front().metadata.id ==
          source.metadata.id);
    REQUIRE(session.undoEdit());
    REQUIRE(session.undoEdit());

    session.close();
    REQUIRE(session.open(notebookPath));
    CHECK(matches("(linked-by " + anchor(source.metadata.id) + ")") ==
          std::vector{targetPage.metadata.id});
}
