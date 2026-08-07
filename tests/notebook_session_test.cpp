// SPDX-License-Identifier: MPL-2.0
#include "hieda/notebook/notebook_session.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

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

TEST_CASE("opening unsupported input returns a typed error") {
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
