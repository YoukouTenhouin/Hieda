// SPDX-License-Identifier: MPL-2.0
#include "notebook_controller.hpp"

#include <catch2/catch_test_macros.hpp>

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

} // namespace

TEST_CASE("the Qt adapter exposes create and close state") {
    TemporaryDirectory temporaryDirectory;
    const auto notebookPath = temporaryDirectory.path() / "adapter.hieda";
    NotebookController controller;

    controller.createNotebook(QUrl::fromLocalFile(QString::fromStdString(notebookPath.string())));

    CHECK(controller.hasOpenNotebook());
    CHECK(controller.notebookName() == QStringLiteral("adapter"));
    CHECK(controller.notebookPath() == QString::fromStdString(notebookPath.string()));
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

    controller.openNotebook(QUrl::fromLocalFile(QString::fromStdString(invalidPath.string())));

    CHECK_FALSE(controller.hasOpenNotebook());
    CHECK(controller.errorMessage() == QStringLiteral("That file is not a valid Hieda Notebook."));
    controller.clearError();
    CHECK(controller.errorMessage().isEmpty());
}
