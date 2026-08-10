// SPDX-License-Identifier: MPL-2.0
#include "notebook_controller.hpp"

#include <catch2/catch_test_macros.hpp>

#include <QAbstractItemModel>
#include <QCoreApplication>
#include <QDate>
#include <QFile>
#include <QThread>
#include <QUrl>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

template <typename Predicate>
auto
waitUntil(Predicate predicate,
          std::chrono::milliseconds timeout = std::chrono::seconds(2)) -> bool
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    QCoreApplication::processEvents();
    return predicate();
}

class TemporaryDirectory {
  public:
    TemporaryDirectory()
    {
        const auto suffix = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        path_ = std::filesystem::temp_directory_path() /
                ("hieda-controller-test-" + suffix);
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

auto
displayPath(const std::filesystem::path& path) -> QString
{
#ifdef _WIN32
    return QString::fromStdWString(path.native());
#else
    return QFile::decodeName(path.c_str());
#endif
}

auto
localFileUrl(const std::filesystem::path& path) -> QUrl
{
    return QUrl::fromLocalFile(displayPath(path));
}

} // namespace

TEST_CASE("the Qt adapter exposes create and close state")
{
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

TEST_CASE("the Qt adapter presents invalid Notebook errors")
{
    TemporaryDirectory temporaryDirectory;
    const auto invalidPath = temporaryDirectory.path() / "invalid.hieda";
    {
        std::ofstream file(invalidPath);
        file << "not a Notebook";
    }
    NotebookController controller;

    controller.openNotebook(localFileUrl(invalidPath));

    CHECK_FALSE(controller.hasOpenNotebook());
    CHECK(controller.errorMessage() ==
          QStringLiteral("That file is not a valid Hieda Notebook."));
    controller.clearError();
    CHECK(controller.errorMessage().isEmpty());
}

TEST_CASE("the Qt adapter preserves a non-ASCII Notebook path")
{
    TemporaryDirectory temporaryDirectory;
    const auto notebookPath =
        temporaryDirectory.path() / std::filesystem::path(u8"筆記.hieda");
    NotebookController controller;

    controller.createNotebook(localFileUrl(notebookPath));

    REQUIRE(controller.hasOpenNotebook());
    CHECK(controller.notebookName() == QString::fromUtf8("筆記"));
    CHECK(controller.notebookPath() == displayPath(notebookPath));
}

TEST_CASE("the Qt adapter presents and durably edits today's flat Journal")
{
    TemporaryDirectory temporaryDirectory;
    const auto notebookPath =
        temporaryDirectory.path() / "journal-adapter.hieda";
    NotebookController controller;
    controller.createNotebook(localFileUrl(notebookPath));
    REQUIRE(controller.hasOpenNotebook());
    CHECK(controller.journalDate() == QDate::currentDate());
    auto* model = controller.outlineEntries();
    REQUIRE(model->rowCount() == 0);

    const auto firstRow =
        controller.insertOutlineEntry(QStringLiteral("first"));
    REQUIRE(firstRow == 0);
    const auto firstIndex = model->index(firstRow, 0);
    const auto firstId =
        model->data(firstIndex, OutlineEntryModel::EntryIdRole).toString();
    QPersistentModelIndex persistentFirst(firstIndex);
    CHECK(model->data(firstIndex, OutlineEntryModel::AuthoredTextRole)
              .toString() == QStringLiteral("first"));

    REQUIRE(controller.insertOutlineEntry(QStringLiteral("third")) == 1);
    REQUIRE(controller.insertOutlineEntry(QString::fromUtf8("第二 🎴"),
                                          firstId) == 1);
    REQUIRE(model->rowCount() == 3);
    CHECK(controller.outlineEntryId(1) ==
          model->data(model->index(1, 0), OutlineEntryModel::EntryIdRole)
              .toString());
    CHECK(controller.outlineEntryId(-1).isEmpty());
    CHECK(persistentFirst.isValid());
    CHECK(persistentFirst.row() == 0);
    CHECK(model->data(model->index(1, 0), OutlineEntryModel::AuthoredTextRole)
              .toString() == QString::fromUtf8("第二 🎴"));

    const auto secondId =
        model->data(model->index(1, 0), OutlineEntryModel::EntryIdRole)
            .toString();
    REQUIRE(
        controller.updateOutlineEntry(secondId, QStringLiteral("  revised  ")));
    CHECK(model->data(model->index(1, 0), OutlineEntryModel::AuthoredTextRole)
              .toString() == QStringLiteral("  revised  "));

    controller.closeNotebook();
    controller.openNotebook(localFileUrl(notebookPath));
    REQUIRE(model->rowCount() == 3);
    CHECK(model->data(model->index(1, 0), OutlineEntryModel::AuthoredTextRole)
              .toString() == QStringLiteral("  revised  "));
}

TEST_CASE("the Qt adapter restores durable text after a rejected edit")
{
    TemporaryDirectory temporaryDirectory;
    NotebookController controller;
    controller.createNotebook(
        localFileUrl(temporaryDirectory.path() / "rejected-adapter.hieda"));
    REQUIRE(controller.insertOutlineEntry(QStringLiteral("durable")) == 0);
    auto* model = controller.outlineEntries();
    const auto id =
        model->data(model->index(0, 0), OutlineEntryModel::EntryIdRole)
            .toString();

    CHECK_FALSE(
        controller.updateOutlineEntry(id, QStringLiteral("carriage\rreturn")));
    CHECK(model->data(model->index(0, 0), OutlineEntryModel::AuthoredTextRole)
              .toString() == QStringLiteral("durable"));
    CHECK_FALSE(controller.errorMessage().isEmpty());
}

TEST_CASE(
    "the Qt adapter switches Journal dates without materializing empty Pages")
{
    TemporaryDirectory temporaryDirectory;
    NotebookController controller;
    controller.createNotebook(
        localFileUrl(temporaryDirectory.path() / "dates.hieda"));
    const auto firstDate = QDate(2026, 8, 7);
    const auto secondDate = firstDate.addDays(1);
    int rolloverRequests = 0;
    QObject::connect(&controller,
                     &NotebookController::journalDateRolloverRequested,
                     &controller, [&]() -> void {
                         ++rolloverRequests;
                         controller.completeJournalDateRollover();
                     });

    controller.requestJournalDateRollover(firstDate);
    REQUIRE(controller.insertOutlineEntry(QStringLiteral("first day")) == 0);
    controller.requestJournalDateRollover(secondDate);
    CHECK(controller.journalDate() == secondDate);
    CHECK(controller.outlineEntries()->rowCount() == 0);
    REQUIRE(controller.insertOutlineEntry(QStringLiteral("second day")) == 0);
    controller.requestJournalDateRollover(firstDate);
    REQUIRE(controller.outlineEntries()->rowCount() == 1);
    CHECK(controller.outlineEntries()
              ->data(controller.outlineEntries()->index(0, 0),
                     OutlineEntryModel::AuthoredTextRole)
              .toString() == QStringLiteral("first day"));
    CHECK(rolloverRequests == 3);
}

TEST_CASE("the Qt adapter creates renames and navigates ordinary Pages")
{
    TemporaryDirectory temporaryDirectory;
    NotebookController controller;
    controller.createNotebook(
        localFileUrl(temporaryDirectory.path() / "pages-adapter.hieda"));

    REQUIRE(controller.createPage(QStringLiteral("project"),
                                  QStringLiteral("Project")));
    CHECK_FALSE(controller.isJournalPage());
    CHECK(controller.currentPageName() == QStringLiteral("project"));
    CHECK(controller.currentPageTitle() == QStringLiteral("Project"));
    CHECK(controller.pageChoices() ==
          QStringList{QStringLiteral("Project — project")});
    CHECK(controller.pageIdForChoice(QStringLiteral("Project — project")) ==
          controller.currentPageId());
    CHECK_FALSE(controller.createPage(QStringLiteral("project"),
                                      QStringLiteral("Duplicate")));
    CHECK_FALSE(controller.errorMessage().isEmpty());
    controller.clearError();
    REQUIRE(controller.insertOutlineEntry(QStringLiteral("page content")) == 0);

    REQUIRE(controller.renameCurrentPage(QStringLiteral("renamed"),
                                         QStringLiteral("Renamed Project")));
    const auto pageId = controller.currentPageId();
    controller.navigateToToday();
    CHECK(controller.isJournalPage());
    controller.navigateToPage(pageId);
    CHECK_FALSE(controller.isJournalPage());
    CHECK(controller.outlineEntries()->rowCount() == 1);
    CHECK(controller.currentPageName() == QStringLiteral("renamed"));

    controller.navigateToJournalDateText(QStringLiteral("2026-08-07"));
    CHECK(controller.isJournalPage());
    CHECK(controller.journalDate() == QDate(2026, 8, 7));
    controller.navigateToJournalDateText(QStringLiteral("not-a-date"));
    CHECK_FALSE(controller.errorMessage().isEmpty());

    controller.clearError();
    controller.navigateToPage(
        QStringLiteral("00000000-0000-0000-0000-000000000001"));
    CHECK(controller.errorMessage().contains(QStringLiteral("Page")));
}

TEST_CASE(
    "the Qt adapter lazily browses and materializes Page Hierarchy previews")
{
    TemporaryDirectory temporaryDirectory;
    NotebookController controller;
    controller.createNotebook(
        localFileUrl(temporaryDirectory.path() / "hierarchy-adapter.hieda"));
    REQUIRE(controller.createPage(QStringLiteral("work/client/alpha"),
                                  QStringLiteral("Alpha")));

    auto* hierarchy = controller.pageHierarchy();
    REQUIRE(hierarchy->rowCount() == 1);
    auto root = hierarchy->index(0, 0);
    CHECK(hierarchy->data(root, PageHierarchyModel::PageNameRole).toString() ==
          QStringLiteral("work"));
    CHECK(hierarchy->data(root, Qt::DisplayRole).toString() ==
          QStringLiteral("work (Page Preview)"));
    CHECK_FALSE(
        hierarchy->data(root, PageHierarchyModel::MaterializedRole).toBool());
    CHECK(hierarchy->data(root, PageHierarchyModel::HasChildrenRole).toBool());
    auto client = hierarchy->index(0, 0, root);
    CHECK(hierarchy->data(client, PageHierarchyModel::LocalSegmentRole)
              .toString() == QStringLiteral("client"));
    CHECK(
        hierarchy->data(client, PageHierarchyModel::PageNameRole).toString() ==
        QStringLiteral("work/client"));
    CHECK(hierarchy->data(client, Qt::DisplayRole).toString() ==
          QStringLiteral("client (Page Preview)"));
    CHECK_FALSE(
        hierarchy->data(client, PageHierarchyModel::MaterializedRole).toBool());
    CHECK(hierarchy->data(client, PageHierarchyModel::AccessibleDescriptionRole)
              .toString()
              .contains(QStringLiteral("Page Preview")));

    controller.navigateToPageName(QStringLiteral("work/client"));
    CHECK(controller.currentPagePreview());
    CHECK_FALSE(controller.isJournalPage());
    CHECK(controller.currentPageName() == QStringLiteral("work/client"));
    CHECK(controller.currentPageId().isEmpty());
    root = hierarchy->index(0, 0);
    client = hierarchy->index(0, 0, root);
    CHECK(hierarchy->data(client, PageHierarchyModel::SelectedRole).toBool());

    REQUIRE(controller.createCurrentPage(QStringLiteral("Client")));
    CHECK_FALSE(controller.currentPagePreview());
    const auto createdId = controller.currentPageId();
    CHECK_FALSE(createdId.isEmpty());
    client = hierarchy->index(0, 0, hierarchy->index(0, 0));
    CHECK(
        hierarchy->data(client, PageHierarchyModel::MaterializedRole).toBool());
    CHECK(hierarchy->data(client, Qt::DisplayRole).toString() ==
          QStringLiteral("Client — client"));
    REQUIRE(controller.deleteCurrentPage());
    CHECK(controller.currentPagePreview());
    CHECK(controller.currentPageName() == QStringLiteral("work/client"));

    REQUIRE(controller.undoOutlineEdit()
                .value(QStringLiteral("succeeded"))
                .toBool());
    CHECK_FALSE(controller.currentPagePreview());
    CHECK(controller.currentPageId() == createdId);
    REQUIRE(controller.redoOutlineEdit()
                .value(QStringLiteral("succeeded"))
                .toBool());
    CHECK(controller.currentPagePreview());
}

TEST_CASE("the Qt adapter follows committed Page Links and presents unresolved "
          "sources")
{
    TemporaryDirectory temporaryDirectory;
    NotebookController controller;
    controller.createNotebook(
        localFileUrl(temporaryDirectory.path() / "page-links-adapter.hieda"));
    REQUIRE(controller.createPage(QStringLiteral("target"),
                                  QStringLiteral("Target")));
    const auto targetId = controller.currentPageId();
    REQUIRE(controller.createPage(QStringLiteral("source"),
                                  QStringLiteral("Source")));
    REQUIRE(controller.insertOutlineEntry(
                QStringLiteral("[[target]] and [[missing/page]]")) == 0);
    const auto sourceId = controller.outlineEntryId(0);
    const auto presentation = controller.committedEntryPresentation(
        sourceId, QStringLiteral("[[target]] and [[missing/page]]"));
    CHECK(presentation.contains(QStringLiteral(">Target</a>")));
    CHECK(presentation.contains(QStringLiteral(">missing/page</a>")));
    CHECK_FALSE(presentation.contains(QStringLiteral("[[target]]")));

    REQUIRE(controller.followPageLink(
        sourceId, 3, QStringLiteral("[[target]] and [[missing/page]]")));
    CHECK(controller.currentPageId() == targetId);
    CHECK(controller.currentPageName() == QStringLiteral("target"));

    controller.navigateToPageName(QStringLiteral("source"));
    REQUIRE(controller.followPageLink(
        sourceId, 20, QStringLiteral("[[target]] and [[missing/page]]")));
    CHECK(controller.currentPagePreview());
    CHECK(controller.currentPageName() == QStringLiteral("missing/page"));
    CHECK(controller.outlineEntries()->rowCount() == 0);
    REQUIRE(controller.pagePreviewSources()->rowCount() == 1);
    CHECK(controller.pagePreviewSources()
              ->data(controller.pagePreviewSources()->index(0, 0),
                     OutlineEntryModel::AuthoredTextRole)
              .toString() == QStringLiteral("[[target]] and [[missing/page]]"));
    REQUIRE(controller.followPagePreviewSource(sourceId));
    CHECK(controller.currentPageName() == QStringLiteral("source"));

    controller.navigateToPageName(QStringLiteral("missing/page"));
    CHECK(controller.currentPagePreview());
    CHECK(controller.outlineEntries()->rowCount() == 0);
    CHECK(controller.pagePreviewSources()->rowCount() == 1);

    controller.navigateToPageName(QStringLiteral("source"));
    CHECK_FALSE(controller.followPageLink(sourceId, 3,
                                          QStringLiteral("draft [[target]]")));
    CHECK(controller.currentPageName() == QStringLiteral("source"));
}

TEST_CASE(
    "the Qt adapter presents dense Unicode Page Links at exact cursor offsets")
{
    TemporaryDirectory temporaryDirectory;
    NotebookController controller;
    controller.createNotebook(
        localFileUrl(temporaryDirectory.path() / "dense-page-links.hieda"));
    REQUIRE(controller.createPage(QStringLiteral("target"),
                                  QStringLiteral("Target")));
    REQUIRE(controller.createPage(QStringLiteral("source"),
                                  QStringLiteral("Source")));
    constexpr auto repetitions = 4096;
    QString authoredText;
    authoredText.reserve(static_cast<qsizetype>(repetitions) * 11);
    for (auto index = 0; index < repetitions; ++index) {
        authoredText += QStringLiteral("β[[target]]");
    }
    REQUIRE(controller.insertOutlineEntry(authoredText) == 0);
    const auto sourceId = controller.outlineEntryId(0);

    const auto presentation =
        controller.committedEntryPresentation(sourceId, authoredText);

    CHECK(presentation.count(QStringLiteral("<a href=")) == repetitions);
    const auto lastCharacterOffset = ((repetitions - 1) * 11) + 1;
    CHECK(presentation.contains(
        QStringLiteral("<a href=\"%1\">Target</a>").arg(lastCharacterOffset)));
}

TEST_CASE("the Qt adapter inserts presents and follows Block References")
{
    TemporaryDirectory temporaryDirectory;
    NotebookController controller;
    controller.createNotebook(localFileUrl(temporaryDirectory.path() /
                                           "block-reference-adapter.hieda"));
    REQUIRE(controller.createPage(QStringLiteral("target"),
                                  QStringLiteral("Target")));
    REQUIRE(controller.insertOutlineEntry(QStringLiteral("target text")) == 0);
    const auto targetPageId = controller.currentPageId();
    const auto targetId = controller.outlineEntryId(0);
    REQUIRE(controller.createPage(QStringLiteral("source"),
                                  QStringLiteral("Source")));
    REQUIRE(controller.insertOutlineEntry(QStringLiteral("before after")) == 0);
    const auto sourceId = controller.outlineEntryId(0);

    REQUIRE(controller.selectBlockReferenceTarget(targetId));
    CHECK(controller.selectedBlockReferenceTargetId() == targetId);
    REQUIRE(controller.insertSelectedBlockReference(
        sourceId, 7, QStringLiteral("before after")));

    const auto notation = NotebookController::blockReferenceNotation(targetId);
    CHECK(controller.outlineEntries()
              ->data(controller.outlineEntries()->index(0, 0),
                     OutlineEntryModel::AuthoredTextRole)
              .toString() ==
          QStringLiteral("before ") + notation + QStringLiteral("after"));
    const auto presentation = controller.committedEntryPresentation(
        sourceId,
        QStringLiteral("before ") + notation + QStringLiteral("after"));
    CHECK(presentation.contains(QStringLiteral(">Block ") + targetId.first(8) +
                                QStringLiteral("</a>")));
    REQUIRE(controller.browseLinkedReferences(targetId));
    CHECK(controller.blockLinkedReferenceTargetId() == targetId);
    CHECK(controller.blockLinkedReferenceTotal() == 1);
    REQUIRE(controller.blockLinkedReferenceSources()->rowCount() == 1);
    CHECK(controller.blockLinkedReferenceSources()
              ->data(controller.blockLinkedReferenceSources()->index(0, 0),
                     OutlineEntryModel::LinkedReferenceContextRole)
              .toString() == QStringLiteral("Top level"));
    CHECK(controller.blockLinkedReferenceSources()
              ->data(controller.blockLinkedReferenceSources()->index(0, 0),
                     OutlineEntryModel::LinkedReferenceGroupRole)
              .toString() == QStringLiteral("Source — source"));
    REQUIRE(controller.followBlockReference(
        sourceId, 10,
        QStringLiteral("before ") + notation + QStringLiteral("after")));
    CHECK(controller.currentPageId() == targetPageId);
    CHECK(controller.identifiedBlockId() == targetId);
    CHECK(controller.linkedReferenceSources()->rowCount() == 0);
}

TEST_CASE("Page and Block Linked Reference views remain independent")
{
    TemporaryDirectory temporaryDirectory;
    NotebookController controller;
    controller.createNotebook(
        localFileUrl(temporaryDirectory.path() / "independent-views.hieda"));
    REQUIRE(controller.createPage(QStringLiteral("page-target"),
                                  QStringLiteral("Page Target")));
    const auto pageTargetId = controller.currentPageId();
    REQUIRE(controller.insertOutlineEntry(QStringLiteral("block target")) == 0);
    const auto blockTargetId = controller.outlineEntryId(0);
    REQUIRE(controller.createPage(QStringLiteral("source"),
                                  QStringLiteral("Source")));
    REQUIRE(controller.insertOutlineEntry(
                QStringLiteral("[[page-target]] [[block:%1]]")
                    .arg(blockTargetId)) == 0);

    controller.navigateToPage(pageTargetId);
    REQUIRE(controller.linkedReferenceSources()->rowCount() == 1);
    REQUIRE(controller.browseLinkedReferences(blockTargetId));

    CHECK(controller.linkedReferenceTargetId() == pageTargetId);
    CHECK(controller.linkedReferenceSources()->rowCount() == 1);
    CHECK(controller.blockLinkedReferenceTargetId() == blockTargetId);
    CHECK(controller.blockLinkedReferenceSources()->rowCount() == 1);
}

TEST_CASE(
    "the Qt adapter incrementally loads Linked Reference occurrence snippets")
{
    TemporaryDirectory temporaryDirectory;
    NotebookController controller;
    controller.createNotebook(
        localFileUrl(temporaryDirectory.path() / "occurrence-snippets.hieda"));
    REQUIRE(controller.createPage(QStringLiteral("target"),
                                  QStringLiteral("Target")));
    const auto targetId = controller.currentPageId();
    REQUIRE(controller.createPage(QStringLiteral("source"),
                                  QStringLiteral("Source")));
    REQUIRE(controller.insertOutlineEntry(QStringLiteral(
                "first [[target]]\nsecond [[target]]\nthird [[target]]\nfourth "
                "context [[target]] tail")) == 0);
    const auto sourceId = controller.outlineEntryId(0);
    controller.navigateToPage(targetId);

    const auto index = controller.linkedReferenceSources()->index(0, 0);
    CHECK(
        controller.linkedReferenceSources()
            ->data(index, OutlineEntryModel::LinkedReferenceOccurrenceCountRole)
            .toLongLong() == 4);
    REQUIRE(controller.linkedReferenceSources()
                ->data(index,
                       OutlineEntryModel::LinkedReferenceHasMoreOccurrencesRole)
                .toBool());
    REQUIRE(controller.loadMoreLinkedReferenceOccurrences(sourceId));
    CHECK_FALSE(
        controller.linkedReferenceSources()
            ->data(index,
                   OutlineEntryModel::LinkedReferenceHasMoreOccurrencesRole)
            .toBool());
    CHECK(controller.linkedReferenceSources()
              ->data(index, OutlineEntryModel::LinkedReferencePresentationRole)
              .toString()
              .contains(QStringLiteral("fourth context")));
}

TEST_CASE(
    "the Qt adapter refreshes a visible Linked Reference view after edits")
{
    TemporaryDirectory temporaryDirectory;
    NotebookController controller;
    controller.createNotebook(localFileUrl(
        temporaryDirectory.path() / "live-linked-reference-adapter.hieda"));
    REQUIRE(controller.createPage(QStringLiteral("target"),
                                  QStringLiteral("Target")));
    const auto targetPageId = controller.currentPageId();

    REQUIRE(controller.insertOutlineEntry(QStringLiteral("self [[target]]")) ==
            0);
    REQUIRE(controller.linkedReferenceSources()->rowCount() == 1);
    const auto sourceId = controller.outlineEntryId(0);
    REQUIRE(controller.updateOutlineEntry(sourceId, QStringLiteral("removed")));
    CHECK(controller.currentPageId() == targetPageId);
    CHECK(controller.linkedReferenceSources()->rowCount() == 0);
    CHECK(controller.linkedReferenceTotal() == 0);
}

TEST_CASE("the Qt adapter browses incoming Page Linked References")
{
    TemporaryDirectory temporaryDirectory;
    NotebookController controller;
    controller.createNotebook(localFileUrl(temporaryDirectory.path() /
                                           "linked-reference-adapter.hieda"));
    REQUIRE(controller.createPage(QStringLiteral("target"),
                                  QStringLiteral("Target")));
    const auto targetPageId = controller.currentPageId();
    REQUIRE(controller.createPage(QStringLiteral("source"),
                                  QStringLiteral("Source")));
    const auto sourcePageId = controller.currentPageId();
    REQUIRE(controller.insertOutlineEntry(
                QStringLiteral("mentions [[target]]")) == 0);
    const auto sourceId = controller.outlineEntryId(0);

    controller.navigateToPage(targetPageId);

    REQUIRE(controller.linkedReferenceSources()->rowCount() == 1);
    CHECK(controller.linkedReferenceSources()
              ->data(controller.linkedReferenceSources()->index(0, 0),
                     OutlineEntryModel::AuthoredTextRole)
              .toString() == QStringLiteral("mentions [[target]]"));
    REQUIRE(controller.followLinkedReferenceSource(sourceId));
    CHECK(controller.currentPageId() == sourcePageId);
}

TEST_CASE("the Qt adapter loads Linked References in bounded batches")
{
    TemporaryDirectory temporaryDirectory;
    NotebookController controller;
    controller.createNotebook(localFileUrl(
        temporaryDirectory.path() / "linked-reference-batches-adapter.hieda"));
    REQUIRE(controller.createPage(QStringLiteral("target"),
                                  QStringLiteral("Target")));
    const auto targetPageId = controller.currentPageId();
    REQUIRE(controller.createPage(QStringLiteral("source"),
                                  QStringLiteral("Source")));
    for (auto index = 0; index < 101; ++index) {
        REQUIRE(controller.insertOutlineEntry(QStringLiteral("[[target]]")) >=
                0);
    }

    controller.navigateToPage(targetPageId);

    CHECK(controller.linkedReferenceTotal() == 101);
    CHECK(controller.linkedReferenceSources()->rowCount() == 100);
    REQUIRE(controller.hasMoreLinkedReferences());
    REQUIRE(controller.loadMoreLinkedReferences());
    CHECK(controller.linkedReferenceSources()->rowCount() == 101);
    CHECK_FALSE(controller.hasMoreLinkedReferences());
}

TEST_CASE("the Qt hierarchy model fetches every revision-bound child batch")
{
    TemporaryDirectory temporaryDirectory;
    NotebookController controller;
    controller.createNotebook(
        localFileUrl(temporaryDirectory.path() / "hierarchy-pages.hieda"));
    for (int index = 0; index < 101; ++index) {
        REQUIRE(
            controller.createPage(QStringLiteral("many/p%1").arg(1000 + index),
                                  QStringLiteral("Many")));
    }
    controller.navigateToToday();
    auto* hierarchy = controller.pageHierarchy();
    const auto many = hierarchy->index(0, 0);
    REQUIRE(many.isValid());
    REQUIRE(hierarchy->canFetchMore(many));
    hierarchy->fetchMore(many);
    CHECK(hierarchy->rowCount(many) == 100);
    REQUIRE(hierarchy->canFetchMore(many));
    hierarchy->fetchMore(many);
    CHECK(hierarchy->rowCount(many) == 101);
    CHECK_FALSE(hierarchy->canFetchMore(many));
}

TEST_CASE("the Qt adapter exposes and edits nested Journal structure")
{
    TemporaryDirectory temporaryDirectory;
    NotebookController controller;
    controller.createNotebook(
        localFileUrl(temporaryDirectory.path() / "nested-adapter.hieda"));
    REQUIRE(controller.insertOutlineEntry(QStringLiteral("parent")) == 0);
    REQUIRE(controller.insertOutlineEntry(QStringLiteral("child")) == 1);
    REQUIRE(controller.insertOutlineEntry(QStringLiteral("tail")) == 2);
    auto* model = controller.outlineEntries();
    const auto childId = controller.outlineEntryId(1);

    const auto indented =
        controller.indentOutlineEntry(childId, QStringLiteral("child"), 2);
    REQUIRE(indented.value(QStringLiteral("succeeded")).toBool());
    CHECK(indented.value(QStringLiteral("row")).toInt() == 1);
    CHECK(indented.value(QStringLiteral("cursorPosition")).toInt() == 2);
    CHECK(
        model->data(model->index(1, 0), OutlineEntryModel::DepthRole).toInt() ==
        1);
    CHECK(model->data(model->index(0, 0), OutlineEntryModel::HasChildrenRole)
              .toBool());
    CHECK_FALSE(
        model->data(model->index(0, 0), OutlineEntryModel::CanDeleteRole)
            .toBool());
    CHECK(model->data(model->index(1, 0), OutlineEntryModel::CanOutdentRole)
              .toBool());

    const auto split =
        controller.splitOutlineEntry(childId, QStringLiteral("child"), 2);
    REQUIRE(split.value(QStringLiteral("succeeded")).toBool());
    const auto splitRow = split.value(QStringLiteral("row")).toInt();
    CHECK(splitRow == 2);
    CHECK(model->data(model->index(1, 0), OutlineEntryModel::AuthoredTextRole)
              .toString() == QStringLiteral("ch"));
    CHECK(model
              ->data(model->index(splitRow, 0),
                     OutlineEntryModel::AuthoredTextRole)
              .toString() == QStringLiteral("ild"));
    CHECK(model->data(model->index(splitRow, 0), OutlineEntryModel::DepthRole)
              .toInt() == 1);

    const auto joined = controller.joinOutlineEntry(
        controller.outlineEntryId(splitRow), QStringLiteral("ild"));
    REQUIRE(joined.value(QStringLiteral("succeeded")).toBool());
    CHECK(joined.value(QStringLiteral("row")).toInt() == 1);
    CHECK(joined.value(QStringLiteral("cursorPosition")).toInt() == 2);
    CHECK(model->data(model->index(1, 0), OutlineEntryModel::AuthoredTextRole)
              .toString() == QStringLiteral("child"));

    const auto rejectedDelete =
        controller.deleteOutlineEntry(controller.outlineEntryId(0));
    CHECK_FALSE(rejectedDelete.value(QStringLiteral("succeeded")).toBool());
    CHECK_FALSE(controller.errorMessage().isEmpty());
    const auto deleted = controller.deleteOutlineEntry(childId);
    REQUIRE(deleted.value(QStringLiteral("succeeded")).toBool());
    CHECK(model->rowCount() == 2);
}

TEST_CASE("the Qt adapter preserves multiline cursor positions and formats "
          "selected subtrees")
{
    TemporaryDirectory temporaryDirectory;
    NotebookController controller;
    controller.createNotebook(
        localFileUrl(temporaryDirectory.path() / "selection-adapter.hieda"));
    REQUIRE(controller.insertOutlineEntry(
                QStringLiteral("parent\ncontinuation")) == 0);
    REQUIRE(controller.insertOutlineEntry(QStringLiteral("child \U0001F3B4")) ==
            1);
    REQUIRE(controller.insertOutlineEntry(QStringLiteral("tail")) == 2);
    const auto parentId = controller.outlineEntryId(0);
    const auto childId = controller.outlineEntryId(1);
    const auto tailId = controller.outlineEntryId(2);
    REQUIRE(
        controller
            .indentOutlineEntry(childId, QStringLiteral("child \U0001F3B4"), 8)
            .value(QStringLiteral("succeeded"))
            .toBool());

    const auto parentSelection = controller.outlineEntrySelection(0, 0);
    CHECK(parentSelection.value(QStringLiteral("roots")).toStringList() ==
          QStringList{parentId});
    CHECK(parentSelection.value(QStringLiteral("entries")).toStringList() ==
          QStringList{parentId, childId});
    const auto extendedSelection = controller.outlineEntrySelection(0, 2);
    CHECK(extendedSelection.value(QStringLiteral("roots")).toStringList() ==
          QStringList{parentId, tailId});
    CHECK(extendedSelection.value(QStringLiteral("entries")).toStringList() ==
          QStringList{parentId, childId, tailId});

    CHECK(controller.outlineSelectionText({parentId, childId, tailId}) ==
          QStringLiteral("\u2022 parent\n  continuation\n  \u2022 child "
                         "\U0001F3B4\n\u2022 tail"));

    const auto split = controller.splitOutlineEntry(
        parentId, QStringLiteral("parent\ncontinuation"), 7);
    REQUIRE(split.value(QStringLiteral("succeeded")).toBool());
    CHECK(controller.outlineEntries()
              ->data(controller.outlineEntries()->index(0, 0),
                     OutlineEntryModel::AuthoredTextRole)
              .toString() == QStringLiteral("parent\n"));
    CHECK(controller.outlineEntries()
              ->data(controller.outlineEntries()->index(2, 0),
                     OutlineEntryModel::AuthoredTextRole)
              .toString() == QStringLiteral("continuation"));
}

TEST_CASE("the Qt adapter cuts selected Journal subtrees and returns "
          "predictable focus")
{
    TemporaryDirectory temporaryDirectory;
    NotebookController controller;
    controller.createNotebook(
        localFileUrl(temporaryDirectory.path() / "cut-adapter.hieda"));
    REQUIRE(controller.insertOutlineEntry(QStringLiteral("parent")) == 0);
    REQUIRE(controller.insertOutlineEntry(QStringLiteral("child")) == 1);
    REQUIRE(controller.insertOutlineEntry(QStringLiteral("tail")) == 2);
    const auto parentId = controller.outlineEntryId(0);
    const auto childId = controller.outlineEntryId(1);
    REQUIRE(controller.indentOutlineEntry(childId, QStringLiteral("child"), 5)
                .value(QStringLiteral("succeeded"))
                .toBool());

    const auto cut = controller.deleteOutlineSubtrees({parentId, childId});

    REQUIRE(cut.value(QStringLiteral("succeeded")).toBool());
    CHECK(cut.value(QStringLiteral("row")).toInt() == 0);
    CHECK(cut.value(QStringLiteral("cursorPosition")).toInt() == 0);
    REQUIRE(controller.outlineEntries()->rowCount() == 1);
    CHECK(controller.outlineEntries()
              ->data(controller.outlineEntries()->index(0, 0),
                     OutlineEntryModel::AuthoredTextRole)
              .toString() == QStringLiteral("tail"));
    REQUIRE(controller.undoOutlineEdit()
                .value(QStringLiteral("succeeded"))
                .toBool());
    CHECK(controller.outlineEntries()->rowCount() == 3);

    REQUIRE(controller.insertOutlineEntry(QStringLiteral("last parent")) == 3);
    REQUIRE(controller.insertOutlineEntry(QStringLiteral("last child")) == 4);
    const auto lastParentId = controller.outlineEntryId(3);
    const auto lastChildId = controller.outlineEntryId(4);
    REQUIRE(
        controller
            .indentOutlineEntry(lastChildId, QStringLiteral("last child"), 10)
            .value(QStringLiteral("succeeded"))
            .toBool());
    const auto trailingCut = controller.deleteOutlineSubtrees({lastParentId});
    REQUIRE(trailingCut.value(QStringLiteral("succeeded")).toBool());
    CHECK(trailingCut.value(QStringLiteral("row")).toInt() == 2);
    CHECK(trailingCut.value(QStringLiteral("cursorPosition")).toInt() == 4);
}

TEST_CASE("the Qt adapter exposes and applies Journal undo and redo")
{
    TemporaryDirectory temporaryDirectory;
    NotebookController controller;
    controller.createNotebook(
        localFileUrl(temporaryDirectory.path() / "undo-adapter.hieda"));
    CHECK_FALSE(controller.canUndo());
    CHECK_FALSE(controller.canRedo());
    REQUIRE(controller.insertOutlineEntry(QStringLiteral("first")) == 0);
    const auto id = controller.outlineEntryId(0);
    REQUIRE(controller.updateOutlineEntry(id, QStringLiteral("changed")));
    CHECK(controller.canUndo());

    const auto undone = controller.undoOutlineEdit();
    REQUIRE(undone.value(QStringLiteral("succeeded")).toBool());
    CHECK(controller.outlineEntries()
              ->data(controller.outlineEntries()->index(0, 0),
                     OutlineEntryModel::AuthoredTextRole)
              .toString() == QStringLiteral("first"));
    CHECK(controller.canRedo());

    const auto redone = controller.redoOutlineEdit();
    REQUIRE(redone.value(QStringLiteral("succeeded")).toBool());
    CHECK(controller.outlineEntries()
              ->data(controller.outlineEntries()->index(0, 0),
                     OutlineEntryModel::AuthoredTextRole)
              .toString() == QStringLiteral("changed"));
    CHECK_FALSE(controller.canRedo());
}

TEST_CASE("the Qt adapter exposes live Query results errors and navigation")
{
    auto argumentCount = 1;
    std::array executableName{'h', 'i', 'e', 'd', 'a', '\0'};
    std::array<char*, 2> arguments{executableName.data(), nullptr};
    QCoreApplication application(argumentCount, arguments.data());
    TemporaryDirectory temporaryDirectory;
    NotebookController controller;
    controller.createNotebook(
        localFileUrl(temporaryDirectory.path() / "query-adapter.hieda"));
    REQUIRE(controller.insertOutlineEntry(QStringLiteral("status::closed")) ==
            0);
    REQUIRE(controller.insertOutlineEntry(QStringLiteral(
                "{{query (where (property-equals status \"open\"))}}")) == 1);
    auto* model = controller.outlineEntries();
    const auto candidateId = controller.outlineEntryId(0);
    const auto queryId = controller.outlineEntryId(1);
    auto queryIndex = model->index(1, 0);

    controller.setQueryActive(queryId, true);
    REQUIRE(waitUntil([&]() -> bool {
        return !model->data(queryIndex, OutlineEntryModel::QueryLoadingRole)
                    .toBool();
    }));

    CHECK(model->data(queryIndex, OutlineEntryModel::QueryHasIntentRole)
              .toBool());
    CHECK(model->data(queryIndex, OutlineEntryModel::QueryErrorRole)
              .toString()
              .isEmpty());
    CHECK(model->data(queryIndex, OutlineEntryModel::QueryResultsRole)
              .toList()
              .isEmpty());

    REQUIRE(controller.updateOutlineEntry(candidateId,
                                          QStringLiteral("status::open")));
    queryIndex = model->index(1, 0);
    REQUIRE(waitUntil([&]() -> bool {
        return model->data(queryIndex, OutlineEntryModel::QueryResultsRole)
                   .toList()
                   .size() == 1;
    }));
    const auto rows =
        model->data(queryIndex, OutlineEntryModel::QueryResultsRole).toList();
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().toMap().value(QStringLiteral("blockId")).toString() ==
          candidateId);
    CHECK_FALSE(
        model->data(queryIndex, OutlineEntryModel::QueryHasMoreRole).toBool());
    CHECK(controller.followQueryResult(candidateId));
    CHECK(controller.identifiedBlockId() == candidateId);

    REQUIRE(controller.updateOutlineEntry(
        queryId, QStringLiteral("{{query (where (type unknown))}}")));
    queryIndex = model->index(1, 0);
    REQUIRE(waitUntil([&]() -> bool {
        return !model->data(queryIndex, OutlineEntryModel::QueryErrorRole)
                    .toString()
                    .isEmpty();
    }));
    CHECK_FALSE(model->data(queryIndex, OutlineEntryModel::QueryErrorRole)
                    .toString()
                    .isEmpty());
    CHECK(model->data(queryIndex, OutlineEntryModel::QueryResultsRole)
              .toList()
              .isEmpty());

    controller.setQueryExpanded(queryId, false);
    CHECK_FALSE(
        model->data(queryIndex, OutlineEntryModel::QueryExpandedRole).toBool());
    CHECK_FALSE(
        model->data(queryIndex, OutlineEntryModel::QueryLoadingRole).toBool());
    controller.setQueryExpanded(queryId, true);
    CHECK(
        model->data(queryIndex, OutlineEntryModel::QueryExpandedRole).toBool());
}
