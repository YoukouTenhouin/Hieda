# Hieda

Hieda is a native Qt Quick notebook application. It creates, closes, and reopens one portable
`.hieda` Notebook at a time and provides a durable nested Journal for the current local date.

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
make lint
make package
```

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

Opening a Notebook shows the Journal Page for the current local date as a nested list of bullets.
The trailing bullet is a temporary draft and an untouched draft is never persisted. Enter splits
an existing Entry at the cursor; Backspace at the start joins a leaf into the previous visible
Entry. Tab and Shift+Tab indent and outdent complete subtrees. Control+Shift+Up/Down on Windows and
Linux, or Command+Shift+Up/Down on macOS, reorder sibling subtrees. Right-click a bullet for the
same structural actions and leaf deletion. Entries with children cannot be joined or deleted.
Escape cancels a text edit, and ordinary Up/Down moves between visible bullets. Long text wraps
visually, but Journal text remains exact single-line Unicode. A failed edit restores the last
durably committed state, while a failed new Entry keeps its draft available for retry or copying.
