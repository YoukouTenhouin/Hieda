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
	cmake -DBUILD_DIR=build/release -DOUTPUT_DIR=build/release/package -DPROJECT_VERSION=0.1.0 -DSOURCE_DIR=. -P cmake/BuildAppImage.cmake
	cmake -DAPPIMAGE=build/release/package/hieda-0.1.0-linux-x86_64.AppImage -P cmake/PackageSmoke.cmake

.PHONY: build test lint package
