# ADR 0006: Cross-platform CI and packaging

## Status

Accepted

## Context

Hieda supports Linux, Windows, and macOS, but a successful source build does not prove that a
desktop application can be launched after its runtime dependencies have been deployed. Release
artifacts also need stable names and a reproducible relationship to the version in CMake.

Linux compatibility is bounded by the oldest distribution used to build the AppImage. Qt's
official Linux binaries target a newer runtime than the selected openSUSE Leap 15.6 baseline, so
the Linux release job cannot share the official Qt binary installation used on Windows and macOS.

## Decision

GitHub Actions builds and tests all three supported platforms on every pull request and push to
`master`. Windows uses MSVC 2022 and produces a portable ZIP. macOS uses an Apple Silicon runner
and produces a DMG containing the application bundle. Linux builds on openSUSE Leap 15.6 and
produces an x86-64 AppImage.

Windows and macOS install Qt 6.8.3 and consume LMDB and Catch2 through the pinned vcpkg manifest.
Linux compiles a checksum-verified Qt 6.8.3 source archive in the Leap container and caches the
installed result. This makes the AppImage's glibc baseline explicit instead of inheriting the
Ubuntu runner's runtime.

Platform deployment tools are used at the packaging boundary: Qt's generated deployment script
drives Windows and macOS deployment, while checksum-pinned linuxdeploy and its Qt plug-in build
the AppImage. Every installed tree and final package is launched with the application's smoke-test
switch.

Packages are generated only for tags. A tag must equal `v` followed by the CMake project version;
after every platform job succeeds, the workflow creates the corresponding GitHub release and
attaches all three packages. Third-party actions are pinned to immutable commits and downloaded
packaging tools and Qt sources are pinned by SHA-256.

## Consequences

The Linux cold-cache job is intentionally expensive because it compiles Qt. Later jobs reuse the
cache, and changing Qt, the compiler, or relevant build options requires changing the cache key.
Unsigned Windows and macOS packages may trigger operating-system trust warnings. Signing and
notarization can be added later without changing the application or package layouts.
