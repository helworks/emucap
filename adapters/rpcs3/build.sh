#!/usr/bin/env bash
# Build the pinned RPCS3 with the emucap producer hooks and install emucap-rpcs3-host.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../_common/build-lock.sh"
. "$HERE/../_common/build-env.sh"
. "$HERE/upstream.lock"
emucap_scrub_build_env

WORK_INPUT="${EMUCAP_RPCS3_WORK:-$HERE/work}"
mkdir -p "$WORK_INPUT"
WORK="$(cd "$WORK_INPUT" && pwd -P)"
emucap_acquire_build_lock "${EMUCAP_BUILD_LOCK:-$WORK/.build.lock}" "rpcs3"
: >"$WORK/.emucap-rpcs3-work"
JOBS="${EMUCAP_BUILD_JOBS:-8}"

SRC="$WORK/rpcs3"
ORIGIN="${EMUCAP_RPCS3_SRC:-$RPCS3_REPO}"
if [ ! -d "$SRC/.git" ]; then
  git clone -q --no-checkout "$ORIGIN" "$SRC"
fi
if ! git -C "$SRC" cat-file -e "$RPCS3_COMMIT^{commit}" 2>/dev/null; then
  git -C "$SRC" fetch -q origin "$RPCS3_COMMIT" || git -C "$SRC" fetch -q origin
fi
git -C "$SRC" checkout -q --force "$RPCS3_COMMIT"
[ "$(git -C "$SRC" rev-parse HEAD)" = "$RPCS3_COMMIT" ] || {
  echo "ERROR: RPCS3 checkout is not the pinned commit" >&2
  exit 1
}
git -C "$SRC" submodule sync -q --recursive
git -C "$SRC" submodule update -q --init --recursive --depth 1
git -C "$SRC" reset -q --hard
git -C "$SRC" clean -qfd -e build-emucap

EMUCAP_REV="$(git -C "$HERE/../.." rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
BUILD_ID="${EMUCAP_REV}@rpcs3-${RPCS3_COMMIT:0:12}"

# The producer and its hooks live in this adapter; the work tree gets copies, upstream is not forked.
mkdir -p "$SRC/rpcs3/Emu/emucap"
cp "$HERE"/host/*.cpp "$HERE"/host/*.h "$HERE/../_common/emucap_producer.h" \
  "$HERE/../_common/emucap_producer.cpp" "$HERE/../_common/emucap_front_auth.h" "$SRC/rpcs3/Emu/emucap/"
printf '#define EMUCAP_BUILD_ID "%s"\n' "$BUILD_ID" >"$SRC/rpcs3/Emu/emucap/emucap_build_id.h"
(cd "$SRC" && python3 "$HERE/host/apply-hooks.py")

BUILD="$SRC/build-emucap"
rm -f "$BUILD/CMakeCache.txt"
cmake -S "$SRC" -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DBUILD_LLVM=OFF -DUSE_NATIVE_INSTRUCTIONS=OFF -DUSE_DISCORD_RPC=OFF \
  -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld -DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld
cmake --build "$BUILD" --target rpcs3 -j "$JOBS"

OUT="$WORK/build"
mkdir -p "$OUT"
install -m 0755 "$HERE/host/emucap-rpcs3-host" "$OUT/emucap-rpcs3-host"
ln -sfn "$BUILD/bin/rpcs3" "$OUT/rpcs3"
printf '%s\n' "$BUILD_ID" >"$OUT/emucap-rpcs3-host.build-id"
echo "built $OUT/emucap-rpcs3-host -> $BUILD/bin/rpcs3 ($BUILD_ID)"
