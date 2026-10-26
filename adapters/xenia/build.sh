#!/usr/bin/env bash
# Build the pinned Xenia Canary with the emucap producer hooks and install emucap-xenia-host.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../_common/build-lock.sh"
. "$HERE/../_common/build-env.sh"
. "$HERE/upstream.lock"
emucap_scrub_build_env

WORK_INPUT="${EMUCAP_XENIA_WORK:-$HERE/work}"
mkdir -p "$WORK_INPUT"
WORK="$(cd "$WORK_INPUT" && pwd -P)"
emucap_acquire_build_lock "${EMUCAP_BUILD_LOCK:-$WORK/.build.lock}" "xenia"
: >"$WORK/.emucap-xenia-work"
JOBS="${EMUCAP_BUILD_JOBS:-8}"

SRC="$WORK/xenia-canary"
ORIGIN="${EMUCAP_XENIA_SRC:-$XENIA_REPO}"
if [ ! -d "$SRC/.git" ]; then
  git clone -q --no-checkout "$ORIGIN" "$SRC"
fi
if ! git -C "$SRC" cat-file -e "$XENIA_COMMIT^{commit}" 2>/dev/null; then
  git -C "$SRC" fetch -q origin "$XENIA_COMMIT" || git -C "$SRC" fetch -q origin
fi
git -C "$SRC" checkout -q --force "$XENIA_COMMIT"
[ "$(git -C "$SRC" rev-parse HEAD)" = "$XENIA_COMMIT" ] || {
  echo "ERROR: Xenia checkout is not the pinned commit" >&2
  exit 1
}
# A local mirror (EMUCAP_XENIA_SRC) keeps upstream submodule URLs, so they come from GitHub.
git -C "$SRC" submodule sync -q --recursive
git -C "$SRC" submodule update -q --init --recursive --depth 1
git -C "$SRC" reset -q --hard
git -C "$SRC" clean -qfd -e build

EMUCAP_REV="$(git -C "$HERE/../.." rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
BUILD_ID="${EMUCAP_REV}@xenia-${XENIA_COMMIT:0:12}"

# The producer and its hooks live in this adapter; the work tree gets copies, upstream is not forked.
mkdir -p "$SRC/src/xenia/app/emucap"
cp "$HERE"/host/*.cpp "$HERE"/host/*.h "$HERE/../_common/emucap_producer.h" \
  "$HERE/../_common/emucap_producer.cpp" "$HERE/../_common/emucap_front_auth.h" "$SRC/src/xenia/app/emucap/"
printf '#define EMUCAP_BUILD_ID "%s"\n' "$BUILD_ID" >"$SRC/src/xenia/app/emucap/emucap_build_id.h"
(cd "$SRC" && python3 "$HERE/host/apply-hooks.py")

# xenia-build.py configures CMake (Ninja Multi-Config in build/) and writes build/version.h.
(cd "$SRC" && CC=clang CXX=clang++ python3 xenia-build.py build --config=release --cc=clang \
  --target=xenia-app -- -j "$JOBS")

BIN="$SRC/build/bin/Linux/Release/xenia_canary"
[ -x "$BIN" ] || { echo "ERROR: $BIN was not built" >&2; exit 1; }
OUT="$WORK/build"
mkdir -p "$OUT"
install -m 0755 "$HERE/host/emucap-xenia-host" "$OUT/emucap-xenia-host"
ln -sfn "$BIN" "$OUT/xenia_canary"
printf '%s\n' "$BUILD_ID" >"$OUT/emucap-xenia-host.build-id"
echo "built $OUT/emucap-xenia-host -> $BIN ($BUILD_ID)"
