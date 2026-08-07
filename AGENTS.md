# Repository Guidelines

## Project Structure & Module Organization

Keep the root uncluttered as the project takes shape. The project follows this conventional layout:

- `src/` for production code, grouped by feature or domain.
- `tests/` for automated tests that mirror the structure under `src/`.
- `assets/` for static files such as fixtures, images, or templates.
- `docs/` for architecture notes and longer contributor documentation.

Document any intentional departure from this layout in this file or the main README.

Public C++ headers live under `include/`, Qt Quick source files under `qml/`, and reusable CMake scripts under `cmake/`; these are conventional tool-specific counterparts to production code under `src/`. Qt adapter behavior is tested directly, while declarative file-dialog wiring receives a packaged-process smoke test rather than brittle native-dialog automation.

## Qt UI Conventions

Toolkit neutrality applies at the Notebook module boundary, not to the presentation. The Qt Quick UI should use Qt Quick Controls without forcing a style so the runtime can select the host platform style. Prefer native dialogs, standard actions, menus, keyboard shortcuts, system fonts, palette colors, and control metrics. Do not build a custom cross-platform skin or hardcode theme colors; introduce custom visuals only when standard controls cannot provide a required capability or accessible interaction. Keep the `QApplication`/Qt Widgets application bootstrap: Linux desktop platform themes use it to supply their integrated file-dialog helper to `QtQuick.Dialogs`.

## Build, Test, and Development Commands

The project uses CMake with Ninja and system-provided dependencies. The canonical commands are:

- `make build` configures and builds the development preset.
- `make test` builds and runs the complete CTest suite.
- `make lint` verifies clang-format and runs clang-tidy with warnings as errors.
- `make package` creates a release TGZ and smoke-tests its installed executable.

Run one test with `ctest --test-dir build/dev -R '<test name>' --output-on-failure`. Required packages and supported tool versions are documented in `README.md`.

## Coding Style & Naming Conventions

Use the formatter and linter standard for the chosen language, committed with repository-local configuration. Run both before submitting changes. Until language-specific rules are established:

- Use spaces rather than tabs and UTF-8 text with a final newline.
- Choose descriptive names; avoid unexplained abbreviations.
- Name files consistently with ecosystem conventions.
- Keep modules focused and expose the smallest practical public interface.

Avoid committing generated output, dependency caches, editor settings, or credentials. Add appropriate patterns to `.gitignore` when tooling is introduced.

## Testing Guidelines

Every behavior change should include automated coverage. Place tests in `tests/` or use the language ecosystem's established colocated convention. Name tests after observable behavior, for example `test_rejects_expired_token`. Include regression tests with bug fixes. Catch2 tests are discovered by CTest; keep behavioral tests at the public Notebook interface and use temporary real Notebook files for persistence coverage.

## Commit & Pull Request Guidelines

Use short, imperative commit subjects such as `Add token validation`, and keep unrelated changes in separate commits. Pull requests should explain the problem and solution, list verification performed, and link relevant issues. Include screenshots or logs for visible UI or operational changes, and call out migrations, compatibility risks, or follow-up work explicitly.
