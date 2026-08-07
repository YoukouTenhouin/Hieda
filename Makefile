build:
	cmake --preset dev
	cmake --build --preset dev

test: build
	ctest --preset dev

lint: build
	clang-format --dry-run --Werror $$(rg --files -g '*.cpp' -g '*.hpp')
	run-clang-tidy -p build/dev '(/src/|/tests/)' -quiet

package:
	cmake --preset release
	cmake --build --preset release
	cpack --config build/release/CPackConfig.cmake -B build/release/package
	cmake -DARCHIVE=build/release/package/hieda-0.1.0-$$(uname -m).tar.gz -P cmake/PackageSmoke.cmake

.PHONY: build test lint package
