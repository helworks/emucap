#!/usr/bin/env bash
# Build the pinned Cemu with the emucap producer hooks and install emucap-cemu-host.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../_common/build-lock.sh"
. "$HERE/../_common/build-env.sh"
. "$HERE/upstream.lock"
emucap_scrub_build_env

WORK_INPUT="${EMUCAP_CEMU_WORK:-$HERE/work}"
mkdir -p "$WORK_INPUT"
WORK="$(cd "$WORK_INPUT" && pwd -P)"
emucap_acquire_build_lock "${EMUCAP_BUILD_LOCK:-$WORK/.build.lock}" "cemu"
: >"$WORK/.emucap-cemu-work"
JOBS="${EMUCAP_BUILD_JOBS:-8}"

SRC="$WORK/Cemu"
ORIGIN="${EMUCAP_CEMU_SRC:-$CEMU_REPO}"
if [ ! -d "$SRC/.git" ]; then
  git clone -q --no-checkout "$ORIGIN" "$SRC"
fi
if ! git -C "$SRC" cat-file -e "$CEMU_COMMIT^{commit}" 2>/dev/null; then
  git -C "$SRC" fetch -q origin "$CEMU_COMMIT" || git -C "$SRC" fetch -q origin
fi
git -C "$SRC" checkout -q --force "$CEMU_COMMIT"
[ "$(git -C "$SRC" rev-parse HEAD)" = "$CEMU_COMMIT" ] || {
  echo "ERROR: Cemu checkout is not the pinned commit" >&2
  exit 1
}
git -C "$SRC" submodule sync -q --recursive
git -C "$SRC" submodule update -q --init --recursive 
git -C "$SRC" reset -q --hard
git -C "$SRC" clean -qfd -e build-emucap

EMUCAP_REV="$(git -C "$HERE/../.." rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
BUILD_ID="${EMUCAP_REV}@cemu-${CEMU_COMMIT:0:12}"

# The producer and its hooks live in this adapter; the work tree gets copies, upstream is not forked.
mkdir -p "$SRC/src/Cafe/emucap"
cp "$HERE"/host/*.cpp "$HERE"/host/*.h "$HERE/../_common/emucap_producer.h" \
  "$HERE/../_common/emucap_producer.cpp" "$HERE/../_common/emucap_front_auth.h" "$SRC/src/Cafe/emucap/"
printf '#define EMUCAP_BUILD_ID "%s"\n' "$BUILD_ID" >"$SRC/src/Cafe/emucap/emucap_build_id.h"
(cd "$SRC" && python3 "$HERE/host/apply-hooks.py")

BUILD="$SRC/build-emucap"
rm -f "$BUILD/CMakeCache.txt"
# Cemu 2.6 needs fmt 10: newer distro fmt rejects its format strings. Build the pinned fmt into
# the work tree once.
FMT_PREFIX="$WORK/fmt-prefix"
if [ ! -f "$FMT_PREFIX/lib/cmake/fmt/fmt-config.cmake" ]; then
  [ -d "$WORK/fmt/.git" ] || git clone -q --depth 1 --branch "$CEMU_FMT_TAG" https://github.com/fmtlib/fmt.git "$WORK/fmt"
  cmake -S "$WORK/fmt" -B "$WORK/fmt/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DFMT_TEST=OFF \
    -DFMT_DOC=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_INSTALL_PREFIX="$FMT_PREFIX"
  cmake --build "$WORK/fmt/build" -j "$JOBS"
  cmake --install "$WORK/fmt/build"
fi
# GCC: clang 21 rejects the fmt consteval format strings in Cemu's precompiled headers. The distro
# hidapi has no CMake package (cmake/hidapiConfig.cmake supplies one), and the distro glslang
# static libraries need SPIRV-Tools appended at link time.
cmake -S "$SRC" -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
  -DENABLE_VCPKG=OFF -DENABLE_DISCORD_RPC=OFF -DENABLE_FERAL_GAMEMODE=OFF -DENABLE_WAYLAND=OFF \
  -DENABLE_HIDAPI=ON -DENABLE_BLUEZ=ON \
  -Dfmt_DIR="$FMT_PREFIX/lib/cmake/fmt" -Dhidapi_DIR="$HERE/cmake" \
  -DCMAKE_CXX_STANDARD_LIBRARIES="-lSPIRV-Tools-opt -lSPIRV-Tools"
cmake --build "$BUILD"  -j "$JOBS"

OUT="$WORK/build"
mkdir -p "$OUT"
install -m 0755 "$HERE/host/emucap-cemu-host" "$OUT/emucap-cemu-host"
ln -sfn "$SRC/bin/Cemu_release" "$OUT/Cemu"
printf '%s\n' "$BUILD_ID" >"$OUT/emucap-cemu-host.build-id"
echo "built $OUT/emucap-cemu-host -> $SRC/bin/Cemu_release ($BUILD_ID)"
