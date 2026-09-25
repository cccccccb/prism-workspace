#!/usr/bin/env bash
set -euo pipefail

# Run after installing generate-ninja, Ninja, and the system font dependencies.
SKIA_CHECKOUT="${PRISM_SKIA_CHECKOUT:-${XDG_CACHE_HOME:-$HOME/.cache}/prism-deps/skia}"
SKIA_REVISION=a004a27085d7dcc4efc3766c9abe92df03654c7c
WUFFS_REVISION=e3f919ccfe3ef542cfc983a82146070258fb57f8
if [[ ! -d "$SKIA_CHECKOUT/.git" ]]; then
    mkdir -p "$(dirname "$SKIA_CHECKOUT")"
    git clone --depth 1 --branch canvaskit/0.39.1 --filter=blob:none \
        https://skia.googlesource.com/skia "$SKIA_CHECKOUT"
fi
if [[ "$(git -C "$SKIA_CHECKOUT" rev-parse HEAD)" != "$SKIA_REVISION" ]]; then
    echo "Skia checkout does not match the pinned revision" >&2
    exit 1
fi
if [[ ! -d "$SKIA_CHECKOUT/third_party/externals/wuffs/.git" ]]; then
    mkdir -p "$SKIA_CHECKOUT/third_party/externals"
    git clone https://skia.googlesource.com/external/github.com/google/wuffs-mirror-release-c.git \
        "$SKIA_CHECKOUT/third_party/externals/wuffs"
    git -C "$SKIA_CHECKOUT/third_party/externals/wuffs" checkout "$WUFFS_REVISION"
fi
if [[ "$(git -C "$SKIA_CHECKOUT/third_party/externals/wuffs" rev-parse HEAD)" != "$WUFFS_REVISION" ]]; then
    echo "Wuffs checkout does not match the pinned revision" >&2
    exit 1
fi
cd "$SKIA_CHECKOUT"
gn gen out/PrismCPU --root=. --args='is_official_build=true is_debug=false skia_enable_tools=false skia_enable_pdf=false skia_enable_svg=false skia_enable_ganesh=false skia_enable_graphite=false skia_use_gl=false skia_use_vulkan=false skia_use_dawn=false skia_use_icu=false skia_use_harfbuzz=false skia_use_fontconfig=true skia_use_freetype=true skia_use_libpng_decode=false skia_use_libpng_encode=true skia_use_libjpeg_turbo_decode=false skia_use_libjpeg_turbo_encode=false skia_use_libwebp_decode=false skia_use_libwebp_encode=false skia_use_expat=false skia_use_zlib=false'
ninja -C out/PrismCPU -j2 skia
printf 'Built %s/out/PrismCPU/libskia.a\n' "$SKIA_CHECKOUT"
