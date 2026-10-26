#!/usr/bin/env bash
# Build the pinned melonDS core with the emucap headless producer (emucap-melonds-host).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../_common/build-lock.sh"
. "$HERE/../_common/build-env.sh"
. "$HERE/upstream.lock"
emucap_scrub_build_env

WORK_INPUT="${EMUCAP_MELONDS_WORK:-$HERE/work}"
mkdir -p "$WORK_INPUT"
WORK="$(cd "$WORK_INPUT" && pwd -P)"
emucap_acquire_build_lock "${EMUCAP_BUILD_LOCK:-$WORK/.build.lock}" "melonds"
: >"$WORK/.emucap-melonds-work"

SRC="$WORK/melonDS"
ORIGIN="${EMUCAP_MELONDS_SRC:-$MELONDS_REPO}"
if [ ! -d "$SRC/.git" ]; then
  git init -q "$SRC"
  git -C "$SRC" remote add origin "$ORIGIN"
fi
git -C "$SRC" remote set-url origin "$ORIGIN"
if ! git -C "$SRC" cat-file -e "$MELONDS_COMMIT^{commit}" 2>/dev/null; then
  git -C "$SRC" fetch -q --depth 1 origin "$MELONDS_COMMIT"
fi
git -C "$SRC" checkout -q --force "$MELONDS_COMMIT"
git -C "$SRC" clean -qfdx
[ "$(git -C "$SRC" rev-parse HEAD)" = "$MELONDS_COMMIT" ] || {
  echo "ERROR: melonDS checkout is not the pinned commit" >&2
  exit 1
}

# The producer is an extra frontend inside the work tree only; upstream sources are not patched.
HOST="$SRC/src/frontend/emucap"
mkdir -p "$HOST"
cp "$HERE/host/emucap_melonds.cpp" "$HERE/host/CMakeLists.txt" "$HOST/"
cp "$HERE/../_common/emucap_producer.h" "$HERE/../_common/emucap_producer.cpp" \
  "$HERE/../_common/emucap_front_auth.h" "$HOST/"
printf '\nadd_subdirectory(src/frontend/emucap)\n' >>"$SRC/CMakeLists.txt"

EMUCAP_REV="$(git -C "$HERE/../.." rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
BUILD_ID="${EMUCAP_REV}@melonds-${MELONDS_COMMIT:0:12}"
BUILD="$WORK/build"
cmake -S "$SRC" -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_QT_SDL=OFF \
  -DENABLE_OGLRENDERER=OFF \
  -DENABLE_GDBSTUB=OFF \
  -DENABLE_LTO_RELEASE=OFF \
  -DEMUCAP_BUILD_ID="$BUILD_ID"
cmake --build "$BUILD" --target emucap-melonds-host -j "${EMUCAP_BUILD_JOBS:-8}"
printf '%s\n' "$BUILD_ID" >"$BUILD/emucap-melonds-host.build-id"
echo "built $BUILD/emucap-melonds-host ($BUILD_ID)"
