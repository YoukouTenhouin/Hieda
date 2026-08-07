# Support the application on three desktop platforms

Status: accepted

Hieda supports 64-bit Linux and Windows builds and Apple Silicon macOS builds. The compiler
baselines are GCC 11, Clang 16, AppleClang 15, and MSVC 2022. Qt remains at 6.8 or newer. Linux may
use system LMDB 0.9.30 or newer and Catch2 3 through their conventional discovery interfaces;
Windows and macOS use the repository's version-pinned vcpkg manifest for LMDB and Catch2 while Qt
is installed separately.

The public `NotebookSession` interface remains toolkit- and operating-system-neutral. Its
implementation uses a private file seam with POSIX and Windows adapters for exclusive ownership,
atomic no-overwrite publication, and durability. LMDB receives native paths on POSIX and UTF-8
paths on Windows, where LMDB converts them to UTF-16. The Qt adapter converts between `QString` and
native `std::filesystem::path` values without passing through a locale-dependent narrow string.

Behavior is tested through the public Notebook interface with real temporary files on each host.
Platform-specific package creation, deployment, and release automation are separate decisions and
do not belong to this compatibility change.
