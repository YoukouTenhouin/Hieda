# Hieda

Hieda is a native Qt Quick notebook application. This first implementation slice creates,
closes, and reopens one portable `.hieda` Notebook at a time.

## Prerequisites

- A 64-bit Linux system
- CMake 3.24 or newer and Ninja
- GCC 11 or newer, or Clang 16 or newer
- Qt 6.8 or newer with Core, GUI, QML, Quick, Quick Controls, and Quick Dialogs
- LMDB 0.9.30 or newer, exposed through `pkg-config`
- Catch2 3
- clang-format, clang-tidy, and ripgrep for linting

Dependencies are supplied by the host distribution. On openSUSE Tumbleweed the relevant
development packages are `qt6-base-devel`, `qt6-declarative-devel`, `qt6-quickcontrols2-devel`,
`lmdb-devel`, and `Catch2-devel`.

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

The TGZ produced by `make package` uses system shared libraries; it is not a self-contained
Linux bundle.

## Notebook files

One closed `.hieda` file contains the canonical Notebook. LMDB and Hieda may create disposable
lock files beside an open Notebook. Do not copy or synchronize a Notebook while it is open.
