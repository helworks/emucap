#!/usr/bin/env bash
# Build the pinned Azahar libretro core (software renderer) and the emucap libretro producer host.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../_common/build-lock.sh"
. "$HERE/../_common/build-env.sh"
. "$HERE/upstream.lock"
emucap_scrub_build_env

WORK_INPUT="${EMUCAP_AZAHAR_WORK:-$HERE/work}"
mkdir -p "$WORK_INPUT"
WORK="$(cd "$WORK_INPUT" && pwd -P)"
emucap_acquire_build_lock "${EMUCAP_BUILD_LOCK:-$WORK/.build.lock}" "azahar"
: >"$WORK/.emucap-azahar-work"
JOBS="${EMUCAP_BUILD_JOBS:-8}"

SRC="$WORK/azahar"
ORIGIN="${EMUCAP_AZAHAR_SRC:-$AZAHAR_REPO}"
if [ ! -d "$SRC/.git" ]; then
  git clone -q --no-checkout "$ORIGIN" "$SRC"
fi
if ! git -C "$SRC" cat-file -e "$AZAHAR_COMMIT^{commit}" 2>/dev/null; then
  git -C "$SRC" fetch -q origin "refs/tags/$AZAHAR_TAG:refs/tags/$AZAHAR_TAG" || git -C "$SRC" fetch -q origin "$AZAHAR_COMMIT"
fi
git -C "$SRC" checkout -q --force "$AZAHAR_COMMIT"
[ "$(git -C "$SRC" rev-parse HEAD)" = "$AZAHAR_COMMIT" ] || {
  echo "ERROR: Azahar checkout is not the pinned commit" >&2
  exit 1
}
# Submodules come from upstream (or from the EMUCAP_AZAHAR_SRC checkout's own modules).
if [ -n "${EMUCAP_AZAHAR_SRC:-}" ] && [ -d "$EMUCAP_AZAHAR_SRC/.git/modules" ] && [ ! -d "$SRC/.git/modules" ]; then
  cp -a "$EMUCAP_AZAHAR_SRC/.git/modules" "$SRC/.git/modules"
fi
git -C "$SRC" submodule sync -q --recursive
git -C "$SRC" submodule update -q --init --recursive --force
git -C "$SRC" reset -q --hard
git -C "$SRC" clean -qfd -e build-emucap
for patch in "$HERE"/patches/*.patch; do
  git -C "$SRC" apply --whitespace=nowarn "$patch"
done

CORE_BUILD="$SRC/build-emucap"
# Re-running the configure step over an existing cache breaks the bundled soundtouch project
# (empty VERSION in set_target_properties), so every build configures with a fresh cache.
# Object files stay; Ninja rebuilds only what changed.
rm -f "$CORE_BUILD/CMakeCache.txt"
cmake -S "$SRC" -B "$CORE_BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_LIBRETRO=ON \
  -DENABLE_OPENGL=OFF \
  -DENABLE_VULKAN=OFF \
  -DENABLE_SOFTWARE_RENDERER=ON \
  -DENABLE_LTO=OFF \
  -DENABLE_TESTS=OFF \
  -DCITRA_WARNINGS_AS_ERRORS=OFF \
  -DCITRA_USE_PRECOMPILED_HEADERS=OFF
cmake --build "$CORE_BUILD" --target azahar_libretro -j "$JOBS"
CORE="$(find "$CORE_BUILD" -name 'azahar_libretro*.so' -type f | head -n 1)"
[ -n "$CORE" ] || { echo "ERROR: azahar_libretro core was not produced" >&2; exit 1; }

if [ "${EMUCAP_AZAHAR_CORE_ONLY:-0}" = "1" ]; then
  echo "built $CORE"
  exit 0
fi

EMUCAP_REV="$(git -C "$HERE/../.." rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
BUILD_ID="${EMUCAP_REV}@azahar-${AZAHAR_COMMIT:0:12}"
OUT="$WORK/build"
cmake -S "$HERE/host" -B "$OUT" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DEMUCAP_COMMON="$HERE/../_common" \
  -DLIBRETRO_INCLUDE="$SRC/externals/libretro-common/libretro-common/include" \
  -DEMUCAP_BUILD_ID="$BUILD_ID"
cmake --build "$OUT" -j "$JOBS"
cp "$CORE" "$OUT/azahar_libretro.so"
printf '%s\n' "$BUILD_ID" >"$OUT/emucap-azahar-host.build-id"
echo "built $OUT/emucap-azahar-host and $OUT/azahar_libretro.so ($BUILD_ID)"
