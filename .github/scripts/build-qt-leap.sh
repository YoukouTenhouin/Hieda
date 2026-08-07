#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
set -euo pipefail

qt_version=6.8.3
qt_sha256=cdd3a69967208276bb01af7ace7dba0ba53e679f886a4cbe624225c60fb73f2c
qt_prefix=${1:?Qt installation prefix is required}

if [[ -x "${qt_prefix}/bin/qmake" ]]; then
    exit 0
fi

work_dir=$(mktemp -d /tmp/hieda-qt-build.XXXXXX)
trap 'rm -rf "${work_dir}"' EXIT

archive="${work_dir}/qt-everywhere-src-${qt_version}.tar.xz"
curl --fail --location --retry 3 \
    "https://download.qt.io/official_releases/qt/6.8/${qt_version}/single/qt-everywhere-src-${qt_version}.tar.xz" \
    --output "${archive}"
echo "${qt_sha256}  ${archive}" | sha256sum --check --status
tar --extract --file "${archive}" --directory "${work_dir}"

export CC=gcc-13
export CXX=g++-13
source_dir="${work_dir}/qt-everywhere-src-${qt_version}"
build_dir="${work_dir}/build"
mkdir -p "${build_dir}"

pushd "${build_dir}" >/dev/null
"${source_dir}/configure" \
    -prefix "${qt_prefix}" \
    -release \
    -opensource \
    -confirm-license \
    -nomake examples \
    -nomake tests \
    -submodules qtbase,qtshadertools,qtdeclarative
popd >/dev/null
cmake --build "${build_dir}" --parallel "$(nproc)"
cmake --install "${build_dir}"
