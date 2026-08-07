// SPDX-License-Identifier: MPL-2.0
#include "hieda/notebook/notebook_session.hpp"

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
