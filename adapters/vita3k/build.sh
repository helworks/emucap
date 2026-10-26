#!/usr/bin/env bash
# Build the pinned Vita3K with the emucap producer hooks and install emucap-vita3k-host.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../_common/build-lock.sh"
. "$HERE/../_common/build-env.sh"
. "$HERE/upstream.lock"
emucap_scrub_build_env

WORK_INPUT="${EMUCAP_VITA3K_WORK:-$HERE/work}"
mkdir -p "$WORK_INPUT"
WORK="$(cd "$WORK_INPUT" && pwd -P)"
emucap_acquire_build_lock "${EMUCAP_BUILD_LOCK:-$WORK/.build.lock}" "vita3k"
: >"$WORK/.emucap-vita3k-work"
JOBS="${EMUCAP_BUILD_JOBS:-8}"

SRC="$WORK/Vita3K"
ORIGIN="${EMUCAP_VITA3K_SRC:-$VITA3K_REPO}"
if [ ! -d "$SRC/.git" ]; then
  git clone -q --no-checkout "$ORIGIN" "$SRC"
fi
if ! git -C "$SRC" cat-file -e "$VITA3K_COMMIT^{commit}" 2>/dev/null; then
  git -C "$SRC" fetch -q origin "$VITA3K_COMMIT" || git -C "$SRC" fetch -q origin
fi
git -C "$SRC" checkout -q --force "$VITA3K_COMMIT"
[ "$(git -C "$SRC" rev-parse HEAD)" = "$VITA3K_COMMIT" ] || {
  echo "ERROR: Vita3K checkout is not the pinned commit" >&2
  exit 1
}
git -C "$SRC" submodule sync -q --recursive
git -C "$SRC" submodule update -q --init --recursive 
git -C "$SRC" reset -q --hard
git -C "$SRC" clean -qfd -e build-emucap

EMUCAP_REV="$(git -C "$HERE/../.." rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
BUILD_ID="${EMUCAP_REV}@vita3k-${VITA3K_COMMIT:0:12}"

# The producer and its hooks live in this adapter; the work tree gets copies, upstream is not forked.
mkdir -p "$SRC/vita3k/sdl-frontend/src"
cp "$HERE"/host/*.cpp "$HERE"/host/*.h "$HERE/../_common/emucap_producer.h" \
  "$HERE/../_common/emucap_producer.cpp" "$HERE/../_common/emucap_front_auth.h" "$SRC/vita3k/sdl-frontend/src/"
printf '#define EMUCAP_BUILD_ID "%s"\n' "$BUILD_ID" >"$SRC/vita3k/sdl-frontend/src/emucap_build_id.h"
(cd "$SRC" && python3 "$HERE/host/apply-hooks.py")

BUILD="$SRC/build-emucap"
rm -f "$BUILD/CMakeCache.txt"
cmake -S "$SRC" -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DUSE_DISCORD_RICH_PRESENCE=OFF
cmake --build "$BUILD" --target vita3k -j "$JOBS"

OUT="$WORK/build"
mkdir -p "$OUT"
install -m 0755 "$HERE/host/emucap-vita3k-host" "$OUT/emucap-vita3k-host"
ln -sfn "$BUILD/bin/Vita3K" "$OUT/Vita3K"
printf '%s\n' "$BUILD_ID" >"$OUT/emucap-vita3k-host.build-id"
echo "built $OUT/emucap-vita3k-host -> $BUILD/bin/Vita3K ($BUILD_ID)"
