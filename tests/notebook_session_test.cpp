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
#include <unistd.h>
#endif

namespace {

class TemporaryDirectory {
  public:
    TemporaryDirectory() {
        const auto suffix =
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        path_ = std::filesystem::temp_directory_path() / ("hieda-test-" + suffix);
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::filesystem::remove_all(path_);
    }

    [[nodiscard]] auto path() const -> const std::filesystem::path& {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

auto lmdbFixturePath(const std::filesystem::path& path) -> std::string {
#ifdef _WIN32
    const auto utf8 = path.u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
#else
    return path.native();
#endif
}

void appendU16(std::vector<std::uint8_t>& output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
    output.push_back(static_cast<std::uint8_t>(value));
}

void appendU32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        output.push_back(static_cast<std::uint8_t>(value >> static_cast<unsigned>(shift)));
    }
}

void appendField(std::vector<std::uint8_t>& output, std::uint16_t tag,
                 const std::vector<std::uint8_t>& value) {
    appendU16(output, tag);
    appendU32(output, static_cast<std::uint32_t>(value.size()));
    output.insert(output.end(), value.begin(), value.end());
}

void createNotebookFixture(const std::filesystem::path& path, std::uint32_t fixtureSchemaVersion,
                           bool includeIdentity = true) {
    MDB_env* environment = nullptr;
    REQUIRE(mdb_env_create(&environment) == MDB_SUCCESS);
    REQUIRE(mdb_env_set_maxdbs(environment, 1) == MDB_SUCCESS);
    const auto encodedPath = lmdbFixturePath(path);
    REQUIRE(mdb_env_open(environment, encodedPath.c_str(), MDB_NOSUBDIR, 0600) == MDB_SUCCESS);
    MDB_txn* transaction = nullptr;
    REQUIRE(mdb_txn_begin(environment, nullptr, 0, &transaction) == MDB_SUCCESS);
    MDB_dbi metadata = 0;
    REQUIRE(mdb_dbi_open(transaction, "metadata", MDB_CREATE, &metadata) == MDB_SUCCESS);

    std::vector<std::uint8_t> manifest;
    appendU16(manifest, 1);
    const std::string_view magic = "HIEDA_NOTEBOOK";
    appendField(manifest, 1, std::vector<std::uint8_t>(magic.begin(), magic.end()));
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

} // namespace

TEST_CASE("a user can create a Notebook at a selected path") {
    TemporaryDirectory temporaryDirectory;
    const auto notebookPath = temporaryDirectory.path() / "ideas.hieda";
    hieda::notebook::NotebookSession session;

    const auto result = session.create(notebookPath);

    REQUIRE(result);
    CHECK(session.isOpen());
    CHECK(result.value().path == notebookPath);
    CHECK(result.value().schemaVersion == 1);
    CHECK(std::filesystem::is_regular_file(notebookPath));
}

TEST_CASE("a created Notebook closes and reopens with the same identity") {
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
    for (const auto& entry : std::filesystem::directory_iterator(temporaryDirectory.path())) {
        if (entry.path().extension() == ".hieda") {
            ++canonicalFiles;
        }
    }
    CHECK(canonicalFiles == 1);
}

TEST_CASE("a Notebook path can contain non-ASCII characters") {
    TemporaryDirectory temporaryDirectory;
    const auto notebookPath = temporaryDirectory.path() / std::filesystem::path(u8"筆記.hieda");
    hieda::notebook::NotebookSession session;

    const auto created = session.create(notebookPath);
    REQUIRE(created);
    CHECK(created.value().path == notebookPath);

    session.close();
    const auto reopened = session.open(notebookPath);
    REQUIRE(reopened);
    CHECK(reopened.value().path == notebookPath);
}

TEST_CASE("creating never overwrites an existing path") {
    TemporaryDirectory temporaryDirectory;
    const auto notebookPath = temporaryDirectory.path() / "existing.hieda";
    {
        std::ofstream existing(notebookPath);
        existing << "keep me";
    }
    hieda::notebook::NotebookSession session;

    const auto result = session.create(notebookPath);

    REQUIRE_FALSE(result);
    CHECK(result.error().code == hieda::notebook::NotebookErrorCode::pathExists);
    std::ifstream existing(notebookPath);
    std::string contents;
    std::getline(existing, contents);
    CHECK(contents == "keep me");
}

TEST_CASE("opening invalid input returns a typed error") {
    TemporaryDirectory temporaryDirectory;
    hieda::notebook::NotebookSession session;

    SECTION("missing path") {
        const auto result = session.open(temporaryDirectory.path() / "missing.hieda");
        REQUIRE_FALSE(result);
        CHECK(result.error().code == hieda::notebook::NotebookErrorCode::pathNotFound);
    }

    SECTION("directory") {
        const auto result = session.open(temporaryDirectory.path());
        REQUIRE_FALSE(result);
        CHECK(result.error().code == hieda::notebook::NotebookErrorCode::invalidPath);
    }

    SECTION("arbitrary file") {
        const auto path = temporaryDirectory.path() / "not-a-notebook.hieda";
        {
            std::ofstream file(path);
            file << "not a Notebook";
        }
        const auto result = session.open(path);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == hieda::notebook::NotebookErrorCode::invalidNotebook);
    }
}

TEST_CASE("opening a newer Notebook schema returns an unsupported-version error") {
    TemporaryDirectory temporaryDirectory;
    const auto notebookPath = temporaryDirectory.path() / "newer.hieda";
    createNotebookFixture(notebookPath, 2);
    hieda::notebook::NotebookSession session;

    const auto result = session.open(notebookPath);

    REQUIRE_FALSE(result);
    CHECK(result.error().code == hieda::notebook::NotebookErrorCode::unsupportedVersion);
}

TEST_CASE("opening an incomplete Notebook manifest returns an invalid error") {
    TemporaryDirectory temporaryDirectory;
    const auto notebookPath = temporaryDirectory.path() / "incomplete.hieda";
    createNotebookFixture(notebookPath, 1, false);
    hieda::notebook::NotebookSession session;

    const auto result = session.open(notebookPath);

    REQUIRE_FALSE(result);
    CHECK(result.error().code == hieda::notebook::NotebookErrorCode::invalidNotebook);
}

TEST_CASE("one session keeps its current Notebook when another open is attempted") {
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
    CHECK(second.error().code == hieda::notebook::NotebookErrorCode::alreadyOpen);
    const auto current = session.current();
    REQUIRE(current);
    const auto currentPath = current ? current->path : std::filesystem::path{};
    CHECK(currentPath == first.value().path);
}

TEST_CASE("a Notebook cannot be owned by two sessions") {
    TemporaryDirectory temporaryDirectory;
    const auto notebookPath = temporaryDirectory.path() / "owned.hieda";
    hieda::notebook::NotebookSession firstSession;
    hieda::notebook::NotebookSession secondSession;
    REQUIRE(firstSession.create(notebookPath));

    const auto result = secondSession.open(notebookPath);

    REQUIRE_FALSE(result);
    CHECK(result.error().code == hieda::notebook::NotebookErrorCode::alreadyInUse);
    firstSession.close();
    REQUIRE(secondSession.open(notebookPath));
}

TEST_CASE("file aliases cannot bypass Notebook ownership") {
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
    CHECK(result.error().code == hieda::notebook::NotebookErrorCode::alreadyInUse);
}

TEST_CASE("destroying a session releases Notebook ownership") {
    TemporaryDirectory temporaryDirectory;
    const auto notebookPath = temporaryDirectory.path() / "lifetime.hieda";
    {
        hieda::notebook::NotebookSession firstSession;
        REQUIRE(firstSession.create(notebookPath));
    }

    hieda::notebook::NotebookSession secondSession;
    REQUIRE(secondSession.open(notebookPath));
}

TEST_CASE("a failed open releases Notebook ownership") {
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
    CHECK(result.error().code == hieda::notebook::NotebookErrorCode::invalidNotebook);
}

TEST_CASE("creating requires an existing parent directory") {
    TemporaryDirectory temporaryDirectory;
    hieda::notebook::NotebookSession session;

    const auto result = session.create(temporaryDirectory.path() / "missing" / "notes.hieda");

    REQUIRE_FALSE(result);
    CHECK(result.error().code == hieda::notebook::NotebookErrorCode::invalidPath);
}

TEST_CASE("creating reports a permission-denied parent directory") {
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
    std::filesystem::permissions(restrictedDirectory, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace);

    REQUIRE_FALSE(result);
    CHECK(result.error().code == hieda::notebook::NotebookErrorCode::permissionDenied);
#endif
}

TEST_CASE("a Journal Page stays virtual until its first Entry is committed") {
    TemporaryDirectory temporaryDirectory;
    const auto notebookPath = temporaryDirectory.path() / "lazy-journal.hieda";
    const hieda::notebook::JournalDate date{2026, 8, 7};
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(notebookPath));

    const auto empty = session.journalPage(date);
    REQUIRE(empty);
    CHECK(empty.value().date == date);
    CHECK_FALSE(empty.value().metadata.has_value());
    CHECK(empty.value().entries.empty());

    session.close();
    REQUIRE(session.open(notebookPath));
    const auto reopened = session.journalPage(date);
    REQUIRE(reopened);
    CHECK_FALSE(reopened.value().metadata.has_value());
}

TEST_CASE("titled Pages preserve unique names identity and contents across rename and reopen") {
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
    CHECK(duplicate.error().code == hieda::notebook::NotebookErrorCode::pageNameConflict);
    CHECK_FALSE(session.createPage("Project", "Invalid name"));
    CHECK_FALSE(session.createPage("empty-title", ""));

    auto page = session.insertPageEntry(pageId, std::nullopt, "parent").value();
    page = session.insertPageEntry(pageId, page.entries.front().metadata.id, "child").value();
    const auto parentId = page.entries.front().metadata.id;
    const auto childId = page.entries.back().metadata.id;
    page = session.movePageEntry(childId, hieda::notebook::PageEntryMove::indent, "child").value();
    CHECK(page.entries.back().parentEntry == parentId);
    CHECK(session.pageEditCapabilities(pageId).value().canUndo);
    const auto undone = session.undoPageEdit(pageId);
    REQUIRE(undone);
    CHECK_FALSE(undone.value().entries.back().parentEntry);
    const auto redone = session.redoPageEdit(pageId);
    REQUIRE(redone);
    CHECK(redone.value().entries.back().parentEntry == parentId);

    const auto renamed = session.renamePage(pageId, "renamed_project", "Renamed Project");
    REQUIRE(renamed);
    CHECK(renamed.value().metadata.id == pageId);
    CHECK(renamed.value().entries.front().metadata.id == parentId);
    CHECK(renamed.value().entries.back().metadata.id == childId);

    session.close();
    REQUIRE(session.open(path));
    const auto reopened = session.page(pageId);
    REQUIRE(reopened);
    CHECK(reopened.value() == renamed.value());
    REQUIRE(session.pages());
    CHECK(session.pages().value().size() == 2);
    CHECK(session.pages().value().front().name == "renamed_project");
}

TEST_CASE("Page Entries provide complete outline commands isolated history and notifications") {
    TemporaryDirectory temporaryDirectory;
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(temporaryDirectory.path() / "page-outline.hieda"));
    const auto firstPage = session.createPage("first", "First").value();
    const auto secondPage = session.createPage("second", "Second").value();
    auto notifications = 0;
    const auto subscription =
        session.subscribeToChanges([&notifications]() -> void { ++notifications; });

    auto outline = session.insertPageEntry(firstPage.metadata.id, std::nullopt, "parent").value();
    const auto parentId = outline.entries.front().metadata.id;
    outline = session.insertPageEntry(firstPage.metadata.id, parentId, "child").value();
    const auto childId = outline.entries.back().metadata.id;
    outline = session.insertPageEntry(firstPage.metadata.id, childId, "tail").value();
    const auto tailId = outline.entries.back().metadata.id;
    REQUIRE(session.updatePageEntry(childId, "child updated"));
    outline = session.splitPageEntry(childId, "child updated", 5).value();
    const auto splitId = outline.entries[2].metadata.id;
    outline = session.joinPageEntry(splitId, " updated").value();
    CHECK(outline.entries[1].authoredText == "child updated");
    outline =
        session.movePageEntry(childId, hieda::notebook::PageEntryMove::indent, "child updated")
            .value();
    CHECK(outline.entries[1].parentEntry == parentId);
    outline =
        session.movePageEntry(childId, hieda::notebook::PageEntryMove::outdent, "child updated")
            .value();
    CHECK_FALSE(outline.entries[1].parentEntry);
    outline = session.movePageEntry(childId, hieda::notebook::PageEntryMove::down, "child updated")
                  .value();
    CHECK(outline.entries.back().metadata.id == childId);
    outline =
        session.movePageEntry(childId, hieda::notebook::PageEntryMove::up, "child updated").value();
    CHECK(outline.entries[1].metadata.id == childId);
    outline = session.deletePageEntry(tailId).value();
    REQUIRE(outline.entries.size() == 2);
    outline = session.deletePageSubtrees({parentId}).value();
    REQUIRE(outline.entries.size() == 1);
    CHECK(outline.entries.front().metadata.id == childId);
    CHECK(notifications == 12);

    const auto invalid =
        session.movePageEntry(childId, hieda::notebook::PageEntryMove::up, "invalid\rtext");
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().code == hieda::notebook::NotebookErrorCode::invalidAuthoredText);
    CHECK(notifications == 12);
    REQUIRE(session.undoPageEdit(firstPage.metadata.id));
    REQUIRE(session.redoPageEdit(firstPage.metadata.id));
    CHECK(notifications == 14);

    REQUIRE(session.insertPageEntry(secondPage.metadata.id, std::nullopt, "independent"));
    CHECK(session.pageEditCapabilities(firstPage.metadata.id).value().canUndo);
    CHECK(session.pageEditCapabilities(secondPage.metadata.id).value().canUndo);
    REQUIRE(session.undoPageEdit(secondPage.metadata.id));
    CHECK(session.page(firstPage.metadata.id).value().entries.size() == 1);

    hieda::notebook::BlockId missingPage;
    missingPage.bytes.front() = std::byte{1};
    const auto missing = session.page(missingPage);
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == hieda::notebook::NotebookErrorCode::pageNotFound);
    CHECK_FALSE(session.createPage("bad_title", "line\nbreak"));
    CHECK_FALSE(session.createPage("bad_utf8", std::string("\xC3", 1)));
}

TEST_CASE("flat Journal Entries preserve identity Unicode text and insertion order") {
    TemporaryDirectory temporaryDirectory;
    const auto notebookPath = temporaryDirectory.path() / "journal.hieda";
    const hieda::notebook::JournalDate date{2026, 8, 7};
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(notebookPath));

    const auto first = session.insertJournalEntry(date, std::nullopt, "first");
    REQUIRE(first);
    REQUIRE(first.value().metadata);
    REQUIRE(first.value().entries.size() == 1);
    const auto firstId = first.value().entries.front().metadata.id;
    CHECK(first.value().entries.front().metadata.createdAt ==
          first.value().entries.front().metadata.updatedAt);

    const auto third = session.insertJournalEntry(date, std::nullopt, "third");
    REQUIRE(third);
    REQUIRE(third.value().entries.size() == 2);
    const auto thirdId = third.value().entries.back().metadata.id;

    const std::string unicodeText = "\xE7\xAC\xAC\xE4\xBA\x8C \xF0\x9F\x8E\xB4";
    const auto second = session.insertJournalEntry(date, firstId, unicodeText);
    REQUIRE(second);
    REQUIRE(second.value().entries.size() == 3);
    CHECK(second.value().entries[0].authoredText == "first");
    CHECK(second.value().entries[1].authoredText == unicodeText);
    CHECK(second.value().entries[2].metadata.id == thirdId);

    const auto& expected = second.value();
    session.close();
    REQUIRE(session.open(notebookPath));
    const auto reopened = session.journalPage(date);
    REQUIRE(reopened);
    CHECK(reopened.value() == expected);
}

TEST_CASE("editing a Journal Entry acknowledges exact multiline Unicode text") {
    TemporaryDirectory temporaryDirectory;
    const auto notebookPath = temporaryDirectory.path() / "edit.hieda";
    const hieda::notebook::JournalDate date{2026, 8, 7};
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(notebookPath));
    const auto inserted = session.insertJournalEntry(date, std::nullopt, "before");
    REQUIRE(inserted);
    const auto entry = inserted.value().entries.front();
    REQUIRE(inserted.value().metadata);
    const auto pageMetadata = inserted.value().metadata.value_or(hieda::notebook::BlockMetadata{});

    const auto updated = session.updateJournalEntry(entry.metadata.id, "  after  ");
    REQUIRE(updated);
    CHECK(updated.value().authoredText == "  after  ");
    CHECK(updated.value().metadata.id == entry.metadata.id);
    CHECK(updated.value().metadata.createdAt == entry.metadata.createdAt);

    const std::string multiline = "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\nsecond line";
    const auto multilineUpdate = session.updateJournalEntry(entry.metadata.id, multiline);
    REQUIRE(multilineUpdate);
    CHECK(multilineUpdate.value().authoredText == multiline);

    const auto rejected = session.updateJournalEntry(entry.metadata.id, "carriage\rreturn");
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == hieda::notebook::NotebookErrorCode::invalidAuthoredText);
    const auto page = session.journalPage(date);
    REQUIRE(page);
    CHECK(page.value().entries.front().authoredText == multiline);
    CHECK(page.value().metadata == pageMetadata);

    session.close();
    REQUIRE(session.open(notebookPath));
    CHECK(session.journalPage(date).value().entries.front().authoredText == multiline);
}

TEST_CASE("Journal commands report closed sessions and invalid insertion points") {
    const hieda::notebook::JournalDate date{2026, 8, 7};
    hieda::notebook::NotebookSession session;

    const auto closed = session.journalPage(date);
    REQUIRE_FALSE(closed);
    CHECK(closed.error().code == hieda::notebook::NotebookErrorCode::notebookNotOpen);

    TemporaryDirectory temporaryDirectory;
    REQUIRE(session.create(temporaryDirectory.path() / "positions.hieda"));
    hieda::notebook::BlockId missing;
    missing.bytes.front() = std::byte{1};
    const auto invalid = session.insertJournalEntry(date, missing, "entry");
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().code == hieda::notebook::NotebookErrorCode::invalidInsertionPoint);
    const auto page = session.journalPage(date);
    REQUIRE(page);
    CHECK_FALSE(page.value().metadata.has_value());
}

TEST_CASE("a rejected Journal commit leaves the acknowledged state intact") {
    TemporaryDirectory temporaryDirectory;
    const auto notebookPath = temporaryDirectory.path() / "rejected-save.hieda";
    const hieda::notebook::JournalDate date{2026, 8, 7};
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(notebookPath));
    const auto inserted = session.insertJournalEntry(date, std::nullopt, "durable");
    REQUIRE(inserted);
    const auto entryId = inserted.value().entries.front().metadata.id;
    const auto revisionBeforeFailure = session.current().value_or({}).revision;

    hieda::notebook::NotebookSessionTestAccess::rejectNextJournalCommit(session);
    const auto rejected = session.updateJournalEntry(entryId, "not committed");

    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == hieda::notebook::NotebookErrorCode::ioFailure);
    CHECK(session.current().value_or({}).revision == revisionBeforeFailure);
    const auto current = session.journalPage(date);
    REQUIRE(current);
    CHECK(current.value().entries.front().authoredText == "durable");
    session.close();
    REQUIRE(session.open(notebookPath));
    const auto reopened = session.journalPage(date);
    REQUIRE(reopened);
    CHECK(reopened.value().entries.front().authoredText == "durable");
}

TEST_CASE("Journal ordering rebalances after repeated insertion at one position") {
    TemporaryDirectory temporaryDirectory;
    const hieda::notebook::JournalDate date{2026, 8, 7};
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(temporaryDirectory.path() / "rebalance.hieda"));
    const auto first = session.insertJournalEntry(date, std::nullopt, "anchor");
    REQUIRE(first);
    const auto anchor = first.value().entries.front().metadata.id;

    for (int index = 0; index < 40; ++index) {
        REQUIRE(session.insertJournalEntry(date, anchor, std::to_string(index)));
    }

    const auto page = session.journalPage(date);
    REQUIRE(page);
    REQUIRE(page.value().entries.size() == 41);
    CHECK(page.value().entries.front().authoredText == "anchor");
    CHECK(page.value().entries[1].authoredText == "39");
    CHECK(page.value().entries.back().authoredText == "0");
}

TEST_CASE("subscribers observe committed Journal changes after the session lock is released") {
    TemporaryDirectory temporaryDirectory;
    const hieda::notebook::JournalDate date{2026, 8, 7};
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(temporaryDirectory.path() / "subscription.hieda"));
    int notifications = 0;
    {
        auto subscription = session.subscribeToChanges([&]() -> void {
            ++notifications;
            REQUIRE(session.journalPage(date));
        });
        REQUIRE(session.insertJournalEntry(date, std::nullopt, "committed"));
        CHECK(notifications == 1);
        const auto currentPage = session.journalPage(date);
        REQUIRE(currentPage);
        const auto entry = currentPage.value().entries.front();
        REQUIRE(session.updateJournalEntry(entry.metadata.id, "changed"));
        CHECK(notifications == 2);
        REQUIRE(session.updateJournalEntry(entry.metadata.id, "changed"));
        CHECK(notifications == 2);
    }
    REQUIRE(session.insertJournalEntry(date, std::nullopt, "after unsubscribe"));
    CHECK(notifications == 2);
}

TEST_CASE("a failing subscriber cannot make a committed Journal command appear rejected") {
    TemporaryDirectory temporaryDirectory;
    const hieda::notebook::JournalDate date{2026, 8, 7};
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(temporaryDirectory.path() / "subscriber-failure.hieda"));
    auto subscription =
        session.subscribeToChanges([]() -> void { throw std::runtime_error("observer failed"); });

    const auto inserted = session.insertJournalEntry(date, std::nullopt, "committed");

    REQUIRE(inserted);
    CHECK(session.current().value_or({}).revision == 1);
    session.close();
    REQUIRE(session.open(temporaryDirectory.path() / "subscriber-failure.hieda"));
    REQUIRE(session.journalPage(date));
}

TEST_CASE("Journal Entry text rejects malformed UTF-8") {
    TemporaryDirectory temporaryDirectory;
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(temporaryDirectory.path() / "utf8.hieda"));
    const std::string malformed{"\xC0\xAF", 2};

    const auto result = session.insertJournalEntry({2026, 8, 7}, std::nullopt, malformed);

    REQUIRE_FALSE(result);
    CHECK(result.error().code == hieda::notebook::NotebookErrorCode::invalidAuthoredText);
}

TEST_CASE("nested Journal Entries preserve ancestry identity and order across reopen") {
    TemporaryDirectory temporaryDirectory;
    const auto notebookPath = temporaryDirectory.path() / "nested.hieda";
    const hieda::notebook::JournalDate date{2026, 8, 8};
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(notebookPath));

    const auto insertedParent = session.insertJournalEntry(date, std::nullopt, "parent");
    REQUIRE(insertedParent);
    const auto parentId = insertedParent.value().entries.front().metadata.id;
    const auto insertedChild = session.insertJournalEntry(date, parentId, "child text");
    REQUIRE(insertedChild);
    const auto childId = insertedChild.value().entries.back().metadata.id;

    const auto indented =
        session.moveJournalEntry(childId, hieda::notebook::JournalEntryMove::indent, "child text");
    REQUIRE(indented);
    REQUIRE(indented.value().entries.size() == 2);
    CHECK(indented.value().entries[0].metadata.id == parentId);
    CHECK(indented.value().entries[1].metadata.id == childId);
    CHECK(indented.value().entries[1].parentEntry == parentId);

    const auto split = session.splitJournalEntry(parentId, "parent", 3);
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
    const auto reopened = session.journalPage(date);
    REQUIRE(reopened);
    CHECK(reopened.value() == expected);
}

TEST_CASE("joining and deleting Entries enforce leaf-only structural changes") {
    TemporaryDirectory temporaryDirectory;
    const hieda::notebook::JournalDate date{2026, 8, 8};
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(temporaryDirectory.path() / "join-delete.hieda"));
    const auto first = session.insertJournalEntry(date, std::nullopt, "one");
    REQUIRE(first);
    const auto firstId = first.value().entries[0].metadata.id;
    const auto second = session.insertJournalEntry(date, firstId, "two");
    REQUIRE(second);
    const auto secondId = second.value().entries[1].metadata.id;
    const auto third = session.insertJournalEntry(date, secondId, "three");
    REQUIRE(third);
    const auto thirdId = third.value().entries[2].metadata.id;
    REQUIRE(session.moveJournalEntry(thirdId, hieda::notebook::JournalEntryMove::indent, "three"));

    const auto rejectedJoin = session.joinJournalEntry(secondId, "two");
    REQUIRE_FALSE(rejectedJoin);
    CHECK(rejectedJoin.error().code == hieda::notebook::NotebookErrorCode::blockHasChildren);
    const auto rejectedDelete = session.deleteJournalEntry(secondId);
    REQUIRE_FALSE(rejectedDelete);
    CHECK(rejectedDelete.error().code == hieda::notebook::NotebookErrorCode::blockHasChildren);

    const auto joined = session.joinJournalEntry(thirdId, "three");
    REQUIRE(joined);
    REQUIRE(joined.value().entries.size() == 2);
    CHECK(joined.value().entries[0].metadata.id == firstId);
    CHECK(joined.value().entries[1].metadata.id == secondId);
    CHECK(joined.value().entries[1].authoredText == "twothree");

    const auto deleted = session.deleteJournalEntry(secondId);
    REQUIRE(deleted);
    REQUIRE(deleted.value().entries.size() == 1);
    CHECK(deleted.value().entries.front().metadata.id == firstId);
}

TEST_CASE("local moves reorder complete Journal subtrees") {
    TemporaryDirectory temporaryDirectory;
    const hieda::notebook::JournalDate date{2026, 8, 8};
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(temporaryDirectory.path() / "moves.hieda"));
    REQUIRE(session.insertJournalEntry(date, std::nullopt, "A"));
    auto page = session.insertJournalEntry(date, std::nullopt, "B");
    REQUIRE(page);
    page = session.insertJournalEntry(date, std::nullopt, "C");
    REQUIRE(page);
    const auto firstId = page.value().entries[0].metadata.id;
    const auto secondId = page.value().entries[1].metadata.id;
    const auto thirdId = page.value().entries[2].metadata.id;

    REQUIRE(session.moveJournalEntry(secondId, hieda::notebook::JournalEntryMove::indent, "B"));
    REQUIRE(session.moveJournalEntry(thirdId, hieda::notebook::JournalEntryMove::indent, "C"));
    page = session.insertJournalEntry(date, secondId, "B child");
    REQUIRE(page);
    const auto bChild = page.value().entries[2].metadata.id;
    REQUIRE(session.moveJournalEntry(bChild, hieda::notebook::JournalEntryMove::indent, "B child"));

    const auto movedDown =
        session.moveJournalEntry(secondId, hieda::notebook::JournalEntryMove::down, "B");
    REQUIRE(movedDown);
    REQUIRE(movedDown.value().entries.size() == 4);
    CHECK(movedDown.value().entries[0].metadata.id == firstId);
    CHECK(movedDown.value().entries[1].metadata.id == thirdId);
    CHECK(movedDown.value().entries[2].metadata.id == secondId);
    CHECK(movedDown.value().entries[3].metadata.id == bChild);
    CHECK(movedDown.value().entries[3].parentEntry == secondId);

    const auto movedUp =
        session.moveJournalEntry(secondId, hieda::notebook::JournalEntryMove::up, "B");
    REQUIRE(movedUp);
    CHECK(movedUp.value().entries[1].metadata.id == secondId);
    CHECK(movedUp.value().entries[2].metadata.id == bChild);
    CHECK(movedUp.value().entries[3].metadata.id == thirdId);

    const auto outdented =
        session.moveJournalEntry(secondId, hieda::notebook::JournalEntryMove::outdent, "B");
    REQUIRE(outdented);
    CHECK(outdented.value().entries[0].metadata.id == firstId);
    CHECK(outdented.value().entries[1].metadata.id == thirdId);
    CHECK(outdented.value().entries[1].parentEntry == firstId);
    CHECK(outdented.value().entries[2].metadata.id == secondId);
    CHECK_FALSE(outdented.value().entries[2].parentEntry);
    CHECK(outdented.value().entries[3].parentEntry == secondId);
}

TEST_CASE("invalid and failed structural edits leave the acknowledged outline intact") {
    TemporaryDirectory temporaryDirectory;
    const auto notebookPath = temporaryDirectory.path() / "atomic-outline.hieda";
    const hieda::notebook::JournalDate date{2026, 8, 8};
    const std::string emojiText = "A \xF0\x9F\x8E\xB4 B";
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(notebookPath));
    const auto inserted = session.insertJournalEntry(date, std::nullopt, emojiText);
    REQUIRE(inserted);
    const auto entryId = inserted.value().entries.front().metadata.id;
    const auto& acknowledged = inserted.value();
    const auto revision = session.current().value_or({}).revision;

    const auto invalidCursor = session.splitJournalEntry(entryId, emojiText, 3);
    REQUIRE_FALSE(invalidCursor);
    CHECK(invalidCursor.error().code == hieda::notebook::NotebookErrorCode::invalidCursorPosition);
    const auto invalidMove =
        session.moveJournalEntry(entryId, hieda::notebook::JournalEntryMove::up, "changed");
    REQUIRE_FALSE(invalidMove);
    CHECK(invalidMove.error().code == hieda::notebook::NotebookErrorCode::invalidStructuralMove);
    CHECK(session.current().value_or({}).revision == revision);
    REQUIRE(session.journalPage(date).value() == acknowledged);

    hieda::notebook::NotebookSessionTestAccess::rejectNextJournalCommit(session);
    const auto failedSplit = session.splitJournalEntry(entryId, emojiText, 2);
    REQUIRE_FALSE(failedSplit);
    CHECK(failedSplit.error().code == hieda::notebook::NotebookErrorCode::ioFailure);
    CHECK(session.current().value_or({}).revision == revision);
    CHECK(session.journalPage(date).value() == acknowledged);
    session.close();
    REQUIRE(session.open(notebookPath));
    CHECK(session.journalPage(date).value() == acknowledged);
}

TEST_CASE("multiline Journal Entries split exactly at Unicode cursor boundaries") {
    TemporaryDirectory temporaryDirectory;
    const hieda::notebook::JournalDate date{2026, 8, 8};
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(temporaryDirectory.path() / "multiline-split.hieda"));
    const std::string text = "first line\nA \xF0\x9F\x8E\xB4 second line";
    const auto inserted = session.insertJournalEntry(date, std::nullopt, text);
    REQUIRE(inserted);
    const auto entryId = inserted.value().entries.front().metadata.id;
    const auto splitOffset = text.find(" second");

    const auto split = session.splitJournalEntry(entryId, text, splitOffset);

    REQUIRE(split);
    REQUIRE(split.value().entries.size() == 2);
    CHECK(split.value().entries[0].authoredText == "first line\nA \xF0\x9F\x8E\xB4");
    CHECK(split.value().entries[1].authoredText == " second line");
}

TEST_CASE("selected Journal subtrees are cut as one durable undoable action") {
    TemporaryDirectory temporaryDirectory;
    const auto notebookPath = temporaryDirectory.path() / "cut-subtrees.hieda";
    const hieda::notebook::JournalDate date{2026, 8, 8};
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(notebookPath));
    REQUIRE(session.insertJournalEntry(date, std::nullopt, "parent"));
    auto page = session.insertJournalEntry(date, std::nullopt, "child");
    REQUIRE(page);
    page = session.insertJournalEntry(date, std::nullopt, "tail");
    REQUIRE(page);
    const auto parentId = page.value().entries[0].metadata.id;
    const auto childId = page.value().entries[1].metadata.id;
    const auto tailId = page.value().entries[2].metadata.id;
    REQUIRE(session.moveJournalEntry(childId, hieda::notebook::JournalEntryMove::indent, "child"));
    const auto before = session.journalPage(date).value();

    const auto cut = session.deleteJournalSubtrees({parentId, childId});

    REQUIRE(cut);
    REQUIRE(cut.value().entries.size() == 1);
    CHECK(cut.value().entries.front().metadata.id == tailId);
    CHECK(session.undoJournalEdit(date).value() == before);
    CHECK(session.redoJournalEdit(date).value() == cut.value());
    session.close();
    REQUIRE(session.open(notebookPath));
    CHECK(session.journalPage(date).value() == cut.value());
}

TEST_CASE("failed Journal subtree deletion preserves content revision and history") {
    TemporaryDirectory temporaryDirectory;
    const hieda::notebook::JournalDate date{2026, 8, 8};
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(temporaryDirectory.path() / "failed-cut-subtrees.hieda"));
    const auto page = session.insertJournalEntry(date, std::nullopt, "kept").value();
    const auto entryId = page.entries.front().metadata.id;
    const auto revision = session.current().value_or(hieda::notebook::NotebookInfo{}).revision;
    hieda::notebook::NotebookSessionTestAccess::rejectNextJournalCommit(session);

    const auto failed = session.deleteJournalSubtrees({entryId});

    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == hieda::notebook::NotebookErrorCode::ioFailure);
    CHECK(session.current().value_or(hieda::notebook::NotebookInfo{}).revision == revision);
    CHECK(session.journalPage(date).value() == page);
    CHECK(session.journalEditCapabilities(date).value().canUndo);
    CHECK_FALSE(session.journalEditCapabilities(date).value().canRedo);
}

TEST_CASE("Journal subtree deletion normalizes duplicates and rejects invalid selections") {
    TemporaryDirectory temporaryDirectory;
    const hieda::notebook::JournalDate firstDate{2026, 8, 8};
    const hieda::notebook::JournalDate secondDate{2026, 8, 9};
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(temporaryDirectory.path() / "subtree-selection-validation.hieda"));
    REQUIRE(session.insertJournalEntry(firstDate, std::nullopt, "parent"));
    auto firstPage = session.insertJournalEntry(firstDate, std::nullopt, "child").value();
    const auto parentId = firstPage.entries[0].metadata.id;
    const auto childId = firstPage.entries[1].metadata.id;
    REQUIRE(session.moveJournalEntry(childId, hieda::notebook::JournalEntryMove::indent, "child"));
    const auto foreignPage =
        session.insertJournalEntry(secondDate, std::nullopt, "foreign").value();
    const auto foreignId = foreignPage.entries.front().metadata.id;
    firstPage = session.journalPage(firstDate).value();

    const auto duplicateCut = session.deleteJournalSubtrees({childId, childId});
    REQUIRE(duplicateCut);
    REQUIRE(duplicateCut.value().entries.size() == 1);
    CHECK(duplicateCut.value().entries.front().metadata.id == parentId);
    REQUIRE(session.undoJournalEdit(firstDate));
    CHECK(session.journalPage(firstDate).value() == firstPage);

    const auto revision = session.current().value_or(hieda::notebook::NotebookInfo{}).revision;
    const auto empty = session.deleteJournalSubtrees({});
    REQUIRE_FALSE(empty);
    CHECK(empty.error().code == hieda::notebook::NotebookErrorCode::invalidStructuralMove);
    hieda::notebook::BlockId missingId;
    missingId.bytes.front() = std::byte{1};
    const auto missing = session.deleteJournalSubtrees({parentId, missingId});
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == hieda::notebook::NotebookErrorCode::blockNotFound);
    const auto crossPage = session.deleteJournalSubtrees({parentId, foreignId});
    REQUIRE_FALSE(crossPage);
    CHECK(crossPage.error().code == hieda::notebook::NotebookErrorCode::blockNotFound);
    CHECK(session.current().value_or(hieda::notebook::NotebookInfo{}).revision == revision);
    CHECK(session.journalPage(firstDate).value() == firstPage);
    CHECK(session.journalPage(secondDate).value() == foreignPage);

    const auto emptied = session.deleteJournalSubtrees({parentId});
    REQUIRE(emptied);
    CHECK(emptied.value().metadata.has_value());
    CHECK(emptied.value().entries.empty());
    CHECK(session.undoJournalEdit(firstDate).value() == firstPage);
}

TEST_CASE("Journal structural commands match a reference outline model") {
    struct ReferenceEntry {
        hieda::notebook::BlockId id;
        std::string text;
        std::optional<hieda::notebook::BlockId> parent;
    };

    TemporaryDirectory temporaryDirectory;
    const hieda::notebook::JournalDate date{2026, 8, 8};
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(temporaryDirectory.path() / "reference-model.hieda"));
    REQUIRE(session.insertJournalEntry(date, std::nullopt, "A"));
    REQUIRE(session.insertJournalEntry(date, std::nullopt, "Bee"));
    REQUIRE(session.insertJournalEntry(date, std::nullopt, "C"));
    auto actual = session.insertJournalEntry(date, std::nullopt, "D");
    REQUIRE(actual);
    const auto firstId = actual.value().entries[0].metadata.id;
    const auto secondId = actual.value().entries[1].metadata.id;
    const auto thirdId = actual.value().entries[2].metadata.id;
    const auto fourthId = actual.value().entries[3].metadata.id;
    std::vector<ReferenceEntry> expected{{firstId, "A", std::nullopt},
                                         {secondId, "Bee", std::nullopt},
                                         {thirdId, "C", std::nullopt},
                                         {fourthId, "D", std::nullopt}};
    const auto checkModel = [&](const hieda::notebook::JournalPage& page) -> void {
        REQUIRE(page.entries.size() == expected.size());
        for (std::size_t index = 0; index < expected.size(); ++index) {
            CHECK(page.entries[index].metadata.id == expected[index].id);
            CHECK(page.entries[index].authoredText == expected[index].text);
            CHECK(page.entries[index].parentEntry == expected[index].parent);
        }
    };
    checkModel(actual.value());

    actual = session.moveJournalEntry(secondId, hieda::notebook::JournalEntryMove::indent, "Bee");
    REQUIRE(actual);
    expected[1].parent = firstId;
    checkModel(actual.value());

    actual = session.moveJournalEntry(thirdId, hieda::notebook::JournalEntryMove::indent, "C");
    REQUIRE(actual);
    expected[2].parent = firstId;
    checkModel(actual.value());
    actual = session.moveJournalEntry(thirdId, hieda::notebook::JournalEntryMove::indent, "C");
    REQUIRE(actual);
    expected[2].parent = secondId;
    checkModel(actual.value());

    actual = session.splitJournalEntry(secondId, "Bee", 1);
    REQUIRE(actual);
    const auto splitId = actual.value().entries[3].metadata.id;
    expected[1].text = "B";
    expected.insert(expected.begin() + 3, {splitId, "ee", firstId});
    checkModel(actual.value());

    actual = session.joinJournalEntry(splitId, "ee");
    REQUIRE(actual);
    expected[2].text = "Cee";
    expected.erase(expected.begin() + 3);
    checkModel(actual.value());

    actual = session.moveJournalEntry(thirdId, hieda::notebook::JournalEntryMove::outdent, "Cee");
    REQUIRE(actual);
    expected[2].parent = firstId;
    checkModel(actual.value());
    actual = session.moveJournalEntry(thirdId, hieda::notebook::JournalEntryMove::up, "Cee");
    REQUIRE(actual);
    std::swap(expected[1], expected[2]);
    checkModel(actual.value());
    actual = session.moveJournalEntry(thirdId, hieda::notebook::JournalEntryMove::down, "Cee");
    REQUIRE(actual);
    std::swap(expected[1], expected[2]);
    checkModel(actual.value());

    actual = session.deleteJournalEntry(thirdId);
    REQUIRE(actual);
    expected.erase(expected.begin() + 2);
    checkModel(actual.value());

    const auto revision = session.current().value_or({}).revision;
    hieda::notebook::NotebookSessionTestAccess::rejectNextJournalCommit(session);
    const auto failedMove =
        session.moveJournalEntry(fourthId, hieda::notebook::JournalEntryMove::up, "dirty D");
    REQUIRE_FALSE(failedMove);
    CHECK(failedMove.error().code == hieda::notebook::NotebookErrorCode::ioFailure);
    CHECK(session.current().value_or({}).revision == revision);
    checkModel(session.journalPage(date).value());

    const auto rejectedParentDelete = session.deleteJournalEntry(firstId);
    REQUIRE_FALSE(rejectedParentDelete);
    CHECK(rejectedParentDelete.error().code ==
          hieda::notebook::NotebookErrorCode::blockHasChildren);
    const auto rejectedOutdent =
        session.moveJournalEntry(fourthId, hieda::notebook::JournalEntryMove::outdent, "dirty D");
    REQUIRE_FALSE(rejectedOutdent);
    CHECK(rejectedOutdent.error().code ==
          hieda::notebook::NotebookErrorCode::invalidStructuralMove);
    CHECK(session.current().value_or({}).revision == revision);
    checkModel(session.journalPage(date).value());
}

TEST_CASE("generated Journal moves preserve preorder and single-parent properties") {
    TemporaryDirectory temporaryDirectory;
    const auto notebookPath = temporaryDirectory.path() / "generated-outline.hieda";
    const hieda::notebook::JournalDate date{2026, 8, 8};
    hieda::notebook::NotebookSession session;
    REQUIRE(session.create(notebookPath));
    for (int index = 0; index < 12; ++index) {
        REQUIRE(session.insertJournalEntry(date, std::nullopt, std::to_string(index)));
    }
    auto page = session.journalPage(date).value();
    for (std::size_t index = 1; index < page.entries.size(); index += 2) {
        const auto result = session.moveJournalEntry(page.entries[index].metadata.id,
                                                     hieda::notebook::JournalEntryMove::indent,
                                                     page.entries[index].authoredText);
        REQUIRE(result);
        page = result.value();
    }
    for (std::size_t operation = 0; operation < 20; ++operation) {
        const auto row = operation % page.entries.size();
        const auto movement = operation % 2 == 0 ? hieda::notebook::JournalEntryMove::down
                                                 : hieda::notebook::JournalEntryMove::up;
        const auto result = session.moveJournalEntry(page.entries[row].metadata.id, movement,
                                                     page.entries[row].authoredText);
        if (result) {
            page = result.value();
        }
        for (std::size_t entryIndex = 0; entryIndex < page.entries.size(); ++entryIndex) {
            const auto& entry = page.entries[entryIndex];
            CHECK(std::count_if(page.entries.begin(), page.entries.end(),
                                [&](const auto& candidate) -> bool {
                                    return candidate.metadata.id == entry.metadata.id;
                                }) == 1);
            if (entry.parentEntry) {
                const auto parent =
                    std::ranges::find_if(page.entries, [&](const auto& candidate) -> bool {
                        return candidate.metadata.id == *entry.parentEntry;
                    });
                REQUIRE(parent != page.entries.end());
                CHECK(std::cmp_less(std::distance(page.entries.begin(), parent), entryIndex));
            }
        }
    }
    session.close();
    REQUIRE(session.open(notebookPath));
    CHECK(session.journalPage(date).value() == page);
}

TEST_CASE("Journal edits undo and redo as coherent user actions") {
    TemporaryDirectory temporaryDirectory;
    hieda::notebook::NotebookSession session;
    const hieda::notebook::JournalDate date{2026, 8, 8};
    REQUIRE(session.create(temporaryDirectory.path() / "undo-redo.hieda"));

    CHECK(session.journalEditCapabilities(date).value() == hieda::notebook::EditCapabilities{});
    auto page = session.insertJournalEntry(date, std::nullopt, "parent").value();
    const auto parentId = page.entries.front().metadata.id;
    page = session.insertJournalEntry(date, parentId, "child").value();
    const auto childId = page.entries.back().metadata.id;
    page =
        session.moveJournalEntry(childId, hieda::notebook::JournalEntryMove::indent, "edited child")
            .value();
    const auto acknowledged = page;

    REQUIRE(session.undoJournalEdit(date));
    page = session.journalPage(date).value();
    REQUIRE(page.entries.size() == 2);
    CHECK(page.entries[1].authoredText == "child");
    CHECK_FALSE(page.entries[1].parentEntry);

    REQUIRE(session.redoJournalEdit(date));
    CHECK(session.journalPage(date).value() == acknowledged);
}

TEST_CASE("every supported Journal command round-trips through history") {
    TemporaryDirectory temporaryDirectory;
    hieda::notebook::NotebookSession session;
    const hieda::notebook::JournalDate date{2026, 8, 8};
    REQUIRE(session.create(temporaryDirectory.path() / "all-history-actions.hieda"));
    std::vector<hieda::notebook::JournalPage> states;
    states.push_back(session.journalPage(date).value());
    const auto remember = [&](hieda::notebook::JournalPage page) -> hieda::notebook::JournalPage {
        states.push_back(page);
        return page;
    };

    auto page = remember(session.insertJournalEntry(date, std::nullopt, "A").value());
    const auto firstId = page.entries[0].metadata.id;
    page = remember(session.insertJournalEntry(date, std::nullopt, "BC").value());
    const auto secondId = page.entries[1].metadata.id;
    page = remember(session.insertJournalEntry(date, std::nullopt, "D").value());
    const auto thirdId = page.entries[2].metadata.id;
    REQUIRE(session.updateJournalEntry(secondId, "B2C"));
    page = remember(session.journalPage(date).value());
    page = remember(session.splitJournalEntry(secondId, "B2C", 2).value());
    const auto splitId = page.entries[2].metadata.id;
    page = remember(session.joinJournalEntry(splitId, "2C").value());
    page = remember(
        session.moveJournalEntry(secondId, hieda::notebook::JournalEntryMove::indent, "B2C")
            .value());
    page = remember(
        session.moveJournalEntry(secondId, hieda::notebook::JournalEntryMove::outdent, "B2C")
            .value());
    page = remember(
        session.moveJournalEntry(secondId, hieda::notebook::JournalEntryMove::down, "B2C").value());
    page = remember(
        session.moveJournalEntry(secondId, hieda::notebook::JournalEntryMove::up, "B2C").value());
    page = remember(session.deleteJournalEntry(thirdId).value());
    CHECK(page.entries.front().metadata.id == firstId);

    for (std::size_t index = states.size() - 1; index > 0; --index) {
        CHECK(session.undoJournalEdit(date).value() == states[index - 1]);
    }
    CHECK_FALSE(session.journalEditCapabilities(date).value().canUndo);
    for (std::size_t index = 1; index < states.size(); ++index) {
        CHECK(session.redoJournalEdit(date).value() == states[index]);
    }
    CHECK_FALSE(session.journalEditCapabilities(date).value().canRedo);
}

TEST_CASE("undo restores deleted identity and redo branches clear only after committed edits") {
    TemporaryDirectory temporaryDirectory;
    hieda::notebook::NotebookSession session;
    const hieda::notebook::JournalDate date{2026, 8, 8};
    REQUIRE(session.create(temporaryDirectory.path() / "undo-delete.hieda"));
    auto page = session.insertJournalEntry(date, std::nullopt, "kept").value();
    const auto entry = page.entries.front();
    REQUIRE(session.deleteJournalEntry(entry.metadata.id));

    page = session.undoJournalEdit(date).value();
    REQUIRE(page.entries.size() == 1);
    CHECK(page.entries.front() == entry);
    CHECK(session.journalEditCapabilities(date).value().canRedo);

    const auto rejected = session.updateJournalEntry(entry.metadata.id, "carriage\rreturn");
    REQUIRE_FALSE(rejected);
    CHECK(session.journalEditCapabilities(date).value().canRedo);
    REQUIRE(session.updateJournalEntry(entry.metadata.id, entry.authoredText));
    CHECK(session.journalEditCapabilities(date).value().canRedo);
    REQUIRE(session.updateJournalEntry(entry.metadata.id, "changed"));
    CHECK_FALSE(session.journalEditCapabilities(date).value().canRedo);
}

TEST_CASE("failed undo leaves acknowledged content revision and history intact") {
    TemporaryDirectory temporaryDirectory;
    hieda::notebook::NotebookSession session;
    const hieda::notebook::JournalDate date{2026, 8, 8};
    REQUIRE(session.create(temporaryDirectory.path() / "failed-undo.hieda"));
    const auto page = session.insertJournalEntry(date, std::nullopt, "durable").value();
    const auto revision = session.current().value_or(hieda::notebook::NotebookInfo{}).revision;
    hieda::notebook::NotebookSessionTestAccess::rejectNextJournalCommit(session);

    const auto failed = session.undoJournalEdit(date);

    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == hieda::notebook::NotebookErrorCode::ioFailure);
    CHECK(session.journalPage(date).value() == page);
    CHECK(session.current().value_or(hieda::notebook::NotebookInfo{}).revision == revision);
    CHECK(session.journalEditCapabilities(date).value().canUndo);
    CHECK_FALSE(session.journalEditCapabilities(date).value().canRedo);
}

TEST_CASE("Journal history is Page-local and clears when the Notebook closes") {
    TemporaryDirectory temporaryDirectory;
    const auto path = temporaryDirectory.path() / "page-history.hieda";
    hieda::notebook::NotebookSession session;
    const hieda::notebook::JournalDate firstDate{2026, 8, 8};
    const hieda::notebook::JournalDate secondDate{2026, 8, 9};
    REQUIRE(session.create(path));
    const auto first = session.insertJournalEntry(firstDate, std::nullopt, "first").value();
    const auto second = session.insertJournalEntry(secondDate, std::nullopt, "second").value();

    const auto virtualPage = session.undoJournalEdit(firstDate).value();
    CHECK_FALSE(virtualPage.metadata);
    CHECK(virtualPage.entries.empty());
    CHECK(session.journalPage(secondDate).value() == second);
    CHECK(session.journalEditCapabilities(firstDate).value().canRedo);
    CHECK(session.journalEditCapabilities(secondDate).value().canUndo);
    const auto restored = session.redoJournalEdit(firstDate).value();
    CHECK(restored == first);

    session.close();
    REQUIRE(session.open(path));
    CHECK(session.journalEditCapabilities(firstDate).value() ==
          hieda::notebook::EditCapabilities{});
    const auto unavailable = session.undoJournalEdit(firstDate);
    REQUIRE_FALSE(unavailable);
    CHECK(unavailable.error().code == hieda::notebook::NotebookErrorCode::undoUnavailable);
}

TEST_CASE("failed redo preserves the undone state and redo capability") {
    TemporaryDirectory temporaryDirectory;
    hieda::notebook::NotebookSession session;
    const hieda::notebook::JournalDate date{2026, 8, 8};
    REQUIRE(session.create(temporaryDirectory.path() / "failed-redo.hieda"));
    const auto committed = session.insertJournalEntry(date, std::nullopt, "durable").value();
    const auto undone = session.undoJournalEdit(date).value();
    const auto revision = session.current().value_or(hieda::notebook::NotebookInfo{}).revision;
    hieda::notebook::NotebookSessionTestAccess::rejectNextJournalCommit(session);

    const auto failed = session.redoJournalEdit(date);

    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == hieda::notebook::NotebookErrorCode::ioFailure);
    CHECK(session.journalPage(date).value() == undone);
    CHECK(session.current().value_or(hieda::notebook::NotebookInfo{}).revision == revision);
    CHECK(session.journalEditCapabilities(date).value().canRedo);
    CHECK(session.redoJournalEdit(date).value() == committed);
}

TEST_CASE("Journal history evicts old actions under its memory budget") {
    TemporaryDirectory temporaryDirectory;
    hieda::notebook::NotebookSession session;
    const hieda::notebook::JournalDate date{2026, 8, 8};
    REQUIRE(session.create(temporaryDirectory.path() / "bounded-history.hieda"));
    const std::string firstText(17ULL * 1024ULL * 1024ULL, 'a');
    const std::string secondText(17ULL * 1024ULL * 1024ULL, 'b');
    const auto inserted = session.insertJournalEntry(date, std::nullopt, firstText).value();
    const auto id = inserted.entries.front().metadata.id;
    REQUIRE(session.updateJournalEntry(id, secondText));

    const auto restored = session.undoJournalEdit(date).value();
    REQUIRE(restored.entries.size() == 1);
    CHECK(restored.entries.front().authoredText == firstText);
    CHECK_FALSE(session.journalEditCapabilities(date).value().canUndo);
    CHECK(session.journalEditCapabilities(date).value().canRedo);
}
