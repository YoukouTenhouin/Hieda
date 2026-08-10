# Hieda

Hieda is a native Qt Quick notebook application. It creates, closes, and reopens one portable
`.hieda` Notebook at a time and provides durable nested Journal and ordinary Page outlines.

## Prerequisites

- A 64-bit Linux or Windows system, or an Apple Silicon Mac
- CMake 3.24 or newer and Ninja
- GCC 11 or newer, Clang 16 or newer, AppleClang 15 or newer, or MSVC 2022
- Qt 6.8 or newer with Core, GUI, Widgets, QML, Quick, Quick Controls, and Quick Dialogs
- LMDB 0.9.30 or newer
- Catch2 3
- clang-format, clang-tidy, and ripgrep for linting

On Linux, dependencies may be supplied by the host distribution; LMDB must be exposed through
`pkg-config`. On openSUSE Tumbleweed the relevant development packages are `qt6-base-devel`,
`qt6-declarative-devel`, `qt6-quickcontrols2-devel`, `lmdb-devel`, and `Catch2-devel`.

On Windows and macOS, install LMDB and Catch2 from the checked-in vcpkg manifest and provide the
vcpkg toolchain file when configuring CMake. Qt is installed separately so its official desktop
tools and QML modules are available to the build.

## Commands

```sh
make build
make test
make format
make format-check
make install-hooks
make lint
make package
```

Run `make install-hooks` once after cloning to enable the repository's pre-commit hook. The hook
rejects commits containing C or C++ files that do not match `.clang-format`.

Run one test by name with:

```sh
ctest --test-dir build/dev -R 'closes and reopens' --output-on-failure
```

On Linux, `make package` creates and smoke-tests an x86-64 AppImage. It downloads checksum-pinned
linuxdeploy tools into `build/release/package/tools`; the build itself still uses the dependencies
listed above.

GitHub Actions builds and tests Windows x86-64, macOS on Apple Silicon, and Linux x86-64. Tags of
the form `v<project version>` additionally publish a portable Windows ZIP, a macOS DMG, and a Linux
AppImage to a GitHub release. The Linux release build uses openSUSE Leap 15.6 as its compatibility
baseline and builds Qt 6.8.3 from a checksum-verified source archive.

## Notebook files

One closed `.hieda` file contains the canonical Notebook. LMDB and Hieda may create disposable
lock files beside an open Notebook. Do not copy or synchronize a Notebook while it is open.

## Journal editing

The sidebar moves between ordinary Pages and Journal dates. Previous, Today, and Next change the
Journal date; the Page menu creates, finds, and renames ordinary Pages. Each Page has a unique
lowercase name for future references and a duplicate-capable Unicode display title. Ordinary Page
outlines use the same keyboard and pointer editing interactions described below.

Opening a Notebook shows the Journal Page for the current local date as a nested list of bullets.
The trailing bullet is a temporary draft and an untouched draft is never persisted. Enter splits
an existing Entry at the cursor; Backspace at the start joins a leaf into the previous visible
Entry. Tab and Shift+Tab indent and outdent complete subtrees. Control+Shift+Up/Down on Windows and
Linux, or Command+Shift+Up/Down on macOS, reorder sibling subtrees. Right-click a bullet for the
same structural actions and leaf deletion. Entries with children cannot be joined or deleted.
Escape cancels a text edit. Long text wraps visually. Shift+Enter inserts an LF line break in the
current Entry; plain Enter still splits the Entry, and multiline clipboard text pastes into one
Entry. Up/Down moves within multiline or
wrapped text and crosses to the adjacent visible bullet only from the first or last visual line.
Journal text remains exact Unicode, and a failed edit restores the last durably committed state,
while a failed new Entry keeps its draft available for retry or copying.

Click a persisted bullet to select its complete subtree. Shift-click or Shift+Up/Down extends that
selection across complete subtrees. Copy produces a human-readable indented bullet list; Cut
removes the selected subtrees as one undoable action. Pasting copied outline text into an editor
creates literal multiline text rather than reconstructing the outline.

Hieda durably saves an existing Entry after one second without typing and immediately before a
focus or structural change. Each such typing group and each structural command is one undoable
action; a structural command also includes its pending text. Use the platform-standard Undo and
Redo shortcuts or the Edit menu. History is maintained separately for each Journal Page, uses a
shared 32 MiB memory budget, and clears when the Notebook is closed.

## Queries and properties

Property lines use `key::value` at the start of a logical line. Keys follow the same lowercase
slash-separated grammar as Page names; values remain exact Unicode strings, including whitespace,
empty values, and duplicates.

An Entry whose trimmed committed text begins with `{{query` declares Query intent. A valid Query
executes a saved live selection; invalid or incomplete intent remains editable and displays a Query
Error without executing. Queries use Hieda's
declarative S-expression syntax to filter Blocks by type, Named or Journal Page context, Journal
Date, exact Authored Text substrings, and Properties. They support nested `and`, `or`, and `not`,
creation/update/Journal Date ordering, and positive limits. Matching Blocks appear as read-only
navigable rows below the Query Entry and refresh after committed Notebook changes. Invalid Query
source remains saved and editable, displays a source-located error, and executes no partial or
backend expression.
