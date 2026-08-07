// SPDX-License-Identifier: MPL-2.0
#include "hieda/notebook/notebook_session.hpp"
#include "notebook_session_test_access.hpp"

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
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

TEST_CASE("editing a Journal Entry acknowledges only valid exact text") {
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

    const auto rejected = session.updateJournalEntry(entry.metadata.id, "two\nlines");
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == hieda::notebook::NotebookErrorCode::invalidAuthoredText);
    const auto page = session.journalPage(date);
    REQUIRE(page);
    CHECK(page.value().entries.front().authoredText == "  after  ");
    CHECK(page.value().metadata == pageMetadata);
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
