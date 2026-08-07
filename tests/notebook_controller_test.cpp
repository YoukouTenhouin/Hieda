// SPDX-License-Identifier: MPL-2.0
#include "notebook_controller.hpp"

#include <catch2/catch_test_macros.hpp>

#include <QAbstractItemModel>
#include <QDate>
#include <QFile>
#include <QUrl>

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
        path_ = std::filesystem::temp_directory_path() / ("hieda-controller-test-" + suffix);
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

auto displayPath(const std::filesystem::path& path) -> QString {
#ifdef _WIN32
    return QString::fromStdWString(path.native());
#else
    return QFile::decodeName(path.c_str());
#endif
}

auto localFileUrl(const std::filesystem::path& path) -> QUrl {
    return QUrl::fromLocalFile(displayPath(path));
}

} // namespace

TEST_CASE("the Qt adapter exposes create and close state") {
    TemporaryDirectory temporaryDirectory;
    const auto notebookPath = temporaryDirectory.path() / "adapter.hieda";
    NotebookController controller;

    controller.createNotebook(localFileUrl(notebookPath));

    CHECK(controller.hasOpenNotebook());
    CHECK(controller.notebookName() == QStringLiteral("adapter"));
    CHECK(controller.notebookPath() == displayPath(notebookPath));
    CHECK(controller.errorMessage().isEmpty());

    controller.closeNotebook();
    CHECK_FALSE(controller.hasOpenNotebook());
    CHECK(controller.notebookPath().isEmpty());
}

TEST_CASE("the Qt adapter presents invalid Notebook errors") {
    TemporaryDirectory temporaryDirectory;
    const auto invalidPath = temporaryDirectory.path() / "invalid.hieda";
    {
        std::ofstream file(invalidPath);
        file << "not a Notebook";
    }
    NotebookController controller;

    controller.openNotebook(localFileUrl(invalidPath));

    CHECK_FALSE(controller.hasOpenNotebook());
    CHECK(controller.errorMessage() == QStringLiteral("That file is not a valid Hieda Notebook."));
    controller.clearError();
    CHECK(controller.errorMessage().isEmpty());
}

TEST_CASE("the Qt adapter preserves a non-ASCII Notebook path") {
    TemporaryDirectory temporaryDirectory;
    const auto notebookPath = temporaryDirectory.path() / std::filesystem::path(u8"筆記.hieda");
    NotebookController controller;

    controller.createNotebook(localFileUrl(notebookPath));

    REQUIRE(controller.hasOpenNotebook());
    CHECK(controller.notebookName() == QString::fromUtf8("筆記"));
    CHECK(controller.notebookPath() == displayPath(notebookPath));
}

TEST_CASE("the Qt adapter presents and durably edits today's flat Journal") {
    TemporaryDirectory temporaryDirectory;
    const auto notebookPath = temporaryDirectory.path() / "journal-adapter.hieda";
    NotebookController controller;
    controller.createNotebook(localFileUrl(notebookPath));
    REQUIRE(controller.hasOpenNotebook());
    CHECK(controller.journalDate() == QDate::currentDate());
    auto* model = controller.journalEntries();
    REQUIRE(model->rowCount() == 0);

    const auto firstRow = controller.insertJournalEntry(QStringLiteral("first"));
    REQUIRE(firstRow == 0);
    const auto firstIndex = model->index(firstRow, 0);
    const auto firstId = model->data(firstIndex, JournalEntryModel::EntryIdRole).toString();
    CHECK(model->data(firstIndex, JournalEntryModel::AuthoredTextRole).toString() ==
          QStringLiteral("first"));

    REQUIRE(controller.insertJournalEntry(QStringLiteral("third")) == 1);
    REQUIRE(controller.insertJournalEntry(QString::fromUtf8("第二 🎴"), firstId) == 1);
    REQUIRE(model->rowCount() == 3);
    CHECK(model->data(model->index(1, 0), JournalEntryModel::AuthoredTextRole).toString() ==
          QString::fromUtf8("第二 🎴"));

    const auto secondId =
        model->data(model->index(1, 0), JournalEntryModel::EntryIdRole).toString();
    REQUIRE(controller.updateJournalEntry(secondId, QStringLiteral("  revised  ")));
    CHECK(model->data(model->index(1, 0), JournalEntryModel::AuthoredTextRole).toString() ==
          QStringLiteral("  revised  "));

    controller.closeNotebook();
    controller.openNotebook(localFileUrl(notebookPath));
    REQUIRE(model->rowCount() == 3);
    CHECK(model->data(model->index(1, 0), JournalEntryModel::AuthoredTextRole).toString() ==
          QStringLiteral("  revised  "));
}

TEST_CASE("the Qt adapter restores durable text after a rejected edit") {
    TemporaryDirectory temporaryDirectory;
    NotebookController controller;
    controller.createNotebook(localFileUrl(temporaryDirectory.path() / "rejected-adapter.hieda"));
    REQUIRE(controller.insertJournalEntry(QStringLiteral("durable")) == 0);
    auto* model = controller.journalEntries();
    const auto id = model->data(model->index(0, 0), JournalEntryModel::EntryIdRole).toString();

    CHECK_FALSE(controller.updateJournalEntry(id, QStringLiteral("two\nlines")));
    CHECK(model->data(model->index(0, 0), JournalEntryModel::AuthoredTextRole).toString() ==
          QStringLiteral("durable"));
    CHECK_FALSE(controller.errorMessage().isEmpty());
}

TEST_CASE("the Qt adapter switches Journal dates without materializing empty Pages") {
    TemporaryDirectory temporaryDirectory;
    NotebookController controller;
    controller.createNotebook(localFileUrl(temporaryDirectory.path() / "dates.hieda"));
    const auto firstDate = QDate(2026, 8, 7);
    const auto secondDate = firstDate.addDays(1);
    int rolloverRequests = 0;
    QObject::connect(&controller, &NotebookController::journalDateRolloverRequested, &controller,
                     [&]() -> void {
                         ++rolloverRequests;
                         controller.completeJournalDateRollover();
                     });

    controller.requestJournalDateRollover(firstDate);
    REQUIRE(controller.insertJournalEntry(QStringLiteral("first day")) == 0);
    controller.requestJournalDateRollover(secondDate);
    CHECK(controller.journalDate() == secondDate);
    CHECK(controller.journalEntries()->rowCount() == 0);
    REQUIRE(controller.insertJournalEntry(QStringLiteral("second day")) == 0);
    controller.requestJournalDateRollover(firstDate);
    REQUIRE(controller.journalEntries()->rowCount() == 1);
    CHECK(controller.journalEntries()
              ->data(controller.journalEntries()->index(0, 0), JournalEntryModel::AuthoredTextRole)
              .toString() == QStringLiteral("first day"));
    CHECK(rolloverRequests == 3);
}
