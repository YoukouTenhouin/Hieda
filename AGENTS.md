# Repository Guidelines

## Project Structure & Module Organization

This repository is currently an empty project skeleton: no application source, test suite, assets, or build manifest has been added yet. Keep the root uncluttered as the project takes shape. Prefer a conventional layout:

- `src/` for production code, grouped by feature or domain.
- `tests/` for automated tests that mirror the structure under `src/`.
- `assets/` for static files such as fixtures, images, or templates.
- `docs/` for architecture notes and longer contributor documentation.

Document any intentional departure from this layout in this file or the main README.

## Build, Test, and Development Commands

No build system or package manager is configured yet. When introducing one, add its manifest and lockfile together, and document the canonical commands here and in `README.md`. Prefer a small, stable command surface, such as `make build`, `make test`, and `make lint`, even when those targets wrap language-specific tools. Contributors should not assume a command succeeds until the required configuration is committed.

## Coding Style & Naming Conventions

Use the formatter and linter standard for the chosen language, committed with repository-local configuration. Run both before submitting changes. Until language-specific rules are established:

- Use spaces rather than tabs and UTF-8 text with a final newline.
- Choose descriptive names; avoid unexplained abbreviations.
- Name files consistently with ecosystem conventions.
- Keep modules focused and expose the smallest practical public interface.

Avoid committing generated output, dependency caches, editor settings, or credentials. Add appropriate patterns to `.gitignore` when tooling is introduced.

## Testing Guidelines

Every behavior change should include automated coverage. Place tests in `tests/` or use the language ecosystem's established colocated convention. Name tests after observable behavior, for example `test_rejects_expired_token`. Include regression tests with bug fixes. Once a test framework is selected, document how to run the full suite and a single test.

## Commit & Pull Request Guidelines

No Git history is available to establish an existing commit convention. Use short, imperative subjects such as `Add token validation`, and keep unrelated changes in separate commits. Pull requests should explain the problem and solution, list verification performed, and link relevant issues. Include screenshots or logs for visible UI or operational changes, and call out migrations, compatibility risks, or follow-up work explicitly.
