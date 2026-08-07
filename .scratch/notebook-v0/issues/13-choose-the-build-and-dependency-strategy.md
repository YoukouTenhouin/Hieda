# Choose the build and dependency strategy

Type: grilling
Status: resolved

## Question

Which C++ standard, compiler baseline, CMake structure, dependency acquisition policy, Qt version baseline, formatting and linting tools, test framework, and initial Linux packaging approach should v0 standardize on?

## Comments

Resolved by [ADR 0003](../../../docs/adr/0003-build-dependencies-and-packaging.md). The baseline is system-provided Qt 6.8+, LMDB 0.9.30+, and Catch2 3 with C++20, CMake/Ninja, repository-local linting, CTest, and a CPack TGZ install artifact.
