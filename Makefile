CPP_SOURCES := $(shell rg --files \
	-g '*.c' -g '*.cc' -g '*.cpp' -g '*.cxx' \
	-g '*.h' -g '*.hh' -g '*.hpp' -g '*.hxx')

build:
	cmake --preset dev
	cmake --build --preset dev

test: build
	ctest --preset dev

format:
	clang-format -i $(CPP_SOURCES)

format-check:
	clang-format --dry-run --Werror $(CPP_SOURCES)

install-hooks:
	git config core.hooksPath .githooks

lint: build format-check
	run-clang-tidy -p build/dev '(/src/|/tests/)' -quiet

package:
	cmake --preset release
	cmake --build --preset release
	cmake -DBUILD_DIR=build/release -DOUTPUT_DIR=build/release/package -DPROJECT_VERSION=0.1.0 -DSOURCE_DIR=. -P cmake/BuildAppImage.cmake
	cmake -DAPPIMAGE=build/release/package/hieda-0.1.0-linux-x86_64.AppImage -P cmake/PackageSmoke.cmake

.PHONY: build test format format-check install-hooks lint package
