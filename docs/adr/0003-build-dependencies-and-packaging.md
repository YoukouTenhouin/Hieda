# Standardize the Linux C++ and Qt build

Status: accepted

Hieda uses C++20, CMake 3.24 or newer, Ninja, GCC 11 or newer or Clang 16 or newer, and Qt 6.8 or newer. Qt, LMDB 0.9.30 or newer, and Catch2 3 are system dependencies discovered at configure time. Repository-local clang-format and clang-tidy configuration defines style and static analysis. CTest is the test runner, CMake install rules define the runtime layout, and CPack produces a dependency-unbundled TGZ for the initial Linux artifact; distro-native or self-contained packaging remains ticket 18 work.

The application is MPL-2.0. Qt is dynamically linked under its applicable open-source terms, LMDB and Catch2 notices are recorded, and packaging must retain the license and notices.
