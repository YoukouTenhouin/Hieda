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
    QPersistentModelIndex persistentFirst(firstIndex);
    CHECK(model->data(firstIndex, JournalEntryModel::AuthoredTextRole).toString() ==
          QStringLiteral("first"));

    REQUIRE(controller.insertJournalEntry(QStringLiteral("third")) == 1);
    REQUIRE(controller.insertJournalEntry(QString::fromUtf8("第二 🎴"), firstId) == 1);
    REQUIRE(model->rowCount() == 3);
    CHECK(controller.journalEntryId(1) ==
          model->data(model->index(1, 0), JournalEntryModel::EntryIdRole).toString());
    CHECK(controller.journalEntryId(-1).isEmpty());
    CHECK(persistentFirst.isValid());
    CHECK(persistentFirst.row() == 0);
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

    CHECK_FALSE(controller.updateJournalEntry(id, QStringLiteral("carriage\rreturn")));
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

TEST_CASE("the Qt adapter exposes and edits nested Journal structure") {
    TemporaryDirectory temporaryDirectory;
    NotebookController controller;
    controller.createNotebook(localFileUrl(temporaryDirectory.path() / "nested-adapter.hieda"));
    REQUIRE(controller.insertJournalEntry(QStringLiteral("parent")) == 0);
    REQUIRE(controller.insertJournalEntry(QStringLiteral("child")) == 1);
    REQUIRE(controller.insertJournalEntry(QStringLiteral("tail")) == 2);
    auto* model = controller.journalEntries();
    const auto childId = controller.journalEntryId(1);

    const auto indented = controller.indentJournalEntry(childId, QStringLiteral("child"), 2);
    REQUIRE(indented.value(QStringLiteral("succeeded")).toBool());
    CHECK(indented.value(QStringLiteral("row")).toInt() == 1);
    CHECK(indented.value(QStringLiteral("cursorPosition")).toInt() == 2);
    CHECK(model->data(model->index(1, 0), JournalEntryModel::DepthRole).toInt() == 1);
    CHECK(model->data(model->index(0, 0), JournalEntryModel::HasChildrenRole).toBool());
    CHECK_FALSE(model->data(model->index(0, 0), JournalEntryModel::CanDeleteRole).toBool());
    CHECK(model->data(model->index(1, 0), JournalEntryModel::CanOutdentRole).toBool());

    const auto split = controller.splitJournalEntry(childId, QStringLiteral("child"), 2);
    REQUIRE(split.value(QStringLiteral("succeeded")).toBool());
    const auto splitRow = split.value(QStringLiteral("row")).toInt();
    CHECK(splitRow == 2);
    CHECK(model->data(model->index(1, 0), JournalEntryModel::AuthoredTextRole).toString() ==
          QStringLiteral("ch"));
    CHECK(model->data(model->index(splitRow, 0), JournalEntryModel::AuthoredTextRole).toString() ==
          QStringLiteral("ild"));
    CHECK(model->data(model->index(splitRow, 0), JournalEntryModel::DepthRole).toInt() == 1);

    const auto joined =
        controller.joinJournalEntry(controller.journalEntryId(splitRow), QStringLiteral("ild"));
    REQUIRE(joined.value(QStringLiteral("succeeded")).toBool());
    CHECK(joined.value(QStringLiteral("row")).toInt() == 1);
    CHECK(joined.value(QStringLiteral("cursorPosition")).toInt() == 2);
    CHECK(model->data(model->index(1, 0), JournalEntryModel::AuthoredTextRole).toString() ==
          QStringLiteral("child"));

    const auto rejectedDelete = controller.deleteJournalEntry(controller.journalEntryId(0));
    CHECK_FALSE(rejectedDelete.value(QStringLiteral("succeeded")).toBool());
    CHECK_FALSE(controller.errorMessage().isEmpty());
    const auto deleted = controller.deleteJournalEntry(childId);
    REQUIRE(deleted.value(QStringLiteral("succeeded")).toBool());
    CHECK(model->rowCount() == 2);
}

TEST_CASE("the Qt adapter preserves multiline cursor positions and formats selected subtrees") {
    TemporaryDirectory temporaryDirectory;
    NotebookController controller;
    controller.createNotebook(localFileUrl(temporaryDirectory.path() / "selection-adapter.hieda"));
    REQUIRE(controller.insertJournalEntry(QStringLiteral("parent\ncontinuation")) == 0);
    REQUIRE(controller.insertJournalEntry(QStringLiteral("child \U0001F3B4")) == 1);
    REQUIRE(controller.insertJournalEntry(QStringLiteral("tail")) == 2);
    const auto parentId = controller.journalEntryId(0);
    const auto childId = controller.journalEntryId(1);
    const auto tailId = controller.journalEntryId(2);
    REQUIRE(controller.indentJournalEntry(childId, QStringLiteral("child \U0001F3B4"), 8)
                .value(QStringLiteral("succeeded"))
                .toBool());

    const auto parentSelection = controller.journalEntrySelection(0, 0);
    CHECK(parentSelection.value(QStringLiteral("roots")).toStringList() == QStringList{parentId});
    CHECK(parentSelection.value(QStringLiteral("entries")).toStringList() ==
          QStringList{parentId, childId});
    const auto extendedSelection = controller.journalEntrySelection(0, 2);
    CHECK(extendedSelection.value(QStringLiteral("roots")).toStringList() ==
          QStringList{parentId, tailId});
    CHECK(extendedSelection.value(QStringLiteral("entries")).toStringList() ==
          QStringList{parentId, childId, tailId});

    CHECK(controller.journalSelectionText({parentId, childId, tailId}) ==
          QStringLiteral("\u2022 parent\n  continuation\n  \u2022 child \U0001F3B4\n\u2022 tail"));

    const auto split =
        controller.splitJournalEntry(parentId, QStringLiteral("parent\ncontinuation"), 7);
    REQUIRE(split.value(QStringLiteral("succeeded")).toBool());
    CHECK(controller.journalEntries()
              ->data(controller.journalEntries()->index(0, 0), JournalEntryModel::AuthoredTextRole)
              .toString() == QStringLiteral("parent\n"));
    CHECK(controller.journalEntries()
              ->data(controller.journalEntries()->index(2, 0), JournalEntryModel::AuthoredTextRole)
              .toString() == QStringLiteral("continuation"));
}

TEST_CASE("the Qt adapter cuts selected Journal subtrees and returns predictable focus") {
    TemporaryDirectory temporaryDirectory;
    NotebookController controller;
    controller.createNotebook(localFileUrl(temporaryDirectory.path() / "cut-adapter.hieda"));
    REQUIRE(controller.insertJournalEntry(QStringLiteral("parent")) == 0);
    REQUIRE(controller.insertJournalEntry(QStringLiteral("child")) == 1);
    REQUIRE(controller.insertJournalEntry(QStringLiteral("tail")) == 2);
    const auto parentId = controller.journalEntryId(0);
    const auto childId = controller.journalEntryId(1);
    REQUIRE(controller.indentJournalEntry(childId, QStringLiteral("child"), 5)
                .value(QStringLiteral("succeeded"))
                .toBool());

    const auto cut = controller.deleteJournalSubtrees({parentId, childId});

    REQUIRE(cut.value(QStringLiteral("succeeded")).toBool());
    CHECK(cut.value(QStringLiteral("row")).toInt() == 0);
    CHECK(cut.value(QStringLiteral("cursorPosition")).toInt() == 0);
    REQUIRE(controller.journalEntries()->rowCount() == 1);
    CHECK(controller.journalEntries()
              ->data(controller.journalEntries()->index(0, 0), JournalEntryModel::AuthoredTextRole)
              .toString() == QStringLiteral("tail"));
    REQUIRE(controller.undoJournalEdit().value(QStringLiteral("succeeded")).toBool());
    CHECK(controller.journalEntries()->rowCount() == 3);

    REQUIRE(controller.insertJournalEntry(QStringLiteral("last parent")) == 3);
    REQUIRE(controller.insertJournalEntry(QStringLiteral("last child")) == 4);
    const auto lastParentId = controller.journalEntryId(3);
    const auto lastChildId = controller.journalEntryId(4);
    REQUIRE(controller.indentJournalEntry(lastChildId, QStringLiteral("last child"), 10)
                .value(QStringLiteral("succeeded"))
                .toBool());
    const auto trailingCut = controller.deleteJournalSubtrees({lastParentId});
    REQUIRE(trailingCut.value(QStringLiteral("succeeded")).toBool());
    CHECK(trailingCut.value(QStringLiteral("row")).toInt() == 2);
    CHECK(trailingCut.value(QStringLiteral("cursorPosition")).toInt() == 4);
}

TEST_CASE("the Qt adapter exposes and applies Journal undo and redo") {
    TemporaryDirectory temporaryDirectory;
    NotebookController controller;
    controller.createNotebook(localFileUrl(temporaryDirectory.path() / "undo-adapter.hieda"));
    CHECK_FALSE(controller.canUndo());
    CHECK_FALSE(controller.canRedo());
    REQUIRE(controller.insertJournalEntry(QStringLiteral("first")) == 0);
    const auto id = controller.journalEntryId(0);
    REQUIRE(controller.updateJournalEntry(id, QStringLiteral("changed")));
    CHECK(controller.canUndo());

    const auto undone = controller.undoJournalEdit();
    REQUIRE(undone.value(QStringLiteral("succeeded")).toBool());
    CHECK(controller.journalEntries()
              ->data(controller.journalEntries()->index(0, 0), JournalEntryModel::AuthoredTextRole)
              .toString() == QStringLiteral("first"));
    CHECK(controller.canRedo());

    const auto redone = controller.redoJournalEdit();
    REQUIRE(redone.value(QStringLiteral("succeeded")).toBool());
    CHECK(controller.journalEntries()
              ->data(controller.journalEntries()->index(0, 0), JournalEntryModel::AuthoredTextRole)
              .toString() == QStringLiteral("changed"));
    CHECK_FALSE(controller.canRedo());
}
