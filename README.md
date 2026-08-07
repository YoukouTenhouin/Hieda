# Hieda

Hieda is a native Qt Quick notebook application. This first implementation slice creates,
closes, and reopens one portable `.hieda` Notebook at a time.

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

The current `make package` command remains a dependency-unbundled Linux TGZ. Cross-platform
distribution packages are handled separately from application compatibility.

## Notebook files

One closed `.hieda` file contains the canonical Notebook. LMDB and Hieda may create disposable
lock files beside an open Notebook. Do not copy or synchronize a Notebook while it is open.
