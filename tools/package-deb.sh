#!/usr/bin/env bash
# Build the deployable GLES desktop; diagnostics stay in separate builds.
set -euo pipefail
repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
output_dir=${1:-"$repo_dir/dist/deb"}
mkdir -p "$output_dir"
output_dir=$(realpath "$output_dir")
skia_root=${PRISM_SKIA_ROOT:-"$HOME/.cache/prism-deps/skia"}
cmake -S "$repo_dir" -B "$repo_dir/build-prod" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
    -DBUILD_TESTING=OFF -DPRISM_ENABLE_GLES=ON \
    -DPRISM_SKIA_ROOT="$skia_root" -DPRISM_SKIA_VARIANT=PrismGLES
cmake --build "$repo_dir/build-prod" --parallel "${PRISM_BUILD_JOBS:-2}"
cpack --config "$repo_dir/build-prod/CPackConfig.cmake" -B "$output_dir"
