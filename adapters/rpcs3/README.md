# emucap: RPCS3 (PlayStation 3) producer

The pinned RPCS3 (`upstream.lock`, the commit of the Windows build v0.0.42-20013) is built with
emucap hooks; the producer from `adapters/_common/emucap_producer` runs inside RPCS3.
`emucap-rpcs3-host` is a wrapper that prepares a generation-owned profile and starts
`rpcs3 --no-gui <content>`.

## Build

```bash
adapters/rpcs3/build.sh              # EMUCAP_RPCS3_SRC=<checkout> reuses local objects
```

System LLVM (`BUILD_LLVM=OFF`), clang. `host/apply-hooks.py` edits the work tree only:

- `RSXThread`: `emucap_pause_after_flips()` exposes RPCS3's pause-after-N-flips frame advance;
  each emulated flip calls `emucap_rpcs3_on_flip`, which counts flips and keeps the displayed
  buffer, read from RSX local memory.
- `cellPad.cpp`: port 0 digital buttons come from the producer while it is connected.
- `rpcs3.cpp`: the producer starts before the Qt event loop.
- `Emu/CMakeLists.txt`: the two emucap sources, compiled with exceptions and without RPCS3's
  warning-as-error set.

## Machine files

PS3 firmware: install the operator's `PS3UPDAT.PUP` once into a directory and pass it as
`EMUCAP_RPCS3_FIRMWARE`:

```bash
adapters/rpcs3/work/build/emucap-rpcs3-host --install-firmware PS3UPDAT.PUP <firmware-dir>
```

`EMUCAP_RPCS3_CACHE` (optional) shares the PPU/SPU LLVM cache across generations.

## Behaviour

- Needs an X display (Xvfb works); `--headless` renders nothing. Vulkan renderer by default (llvmpipe OpenGL lacks GL_ARB_bindless_texture, which the RPCS3 GL backend needs)
  (`EMUCAP_RPCS3_RENDERER`), Write Color Buffers on.
- Self-paced producer: requests run on the producer's service thread. A frozen stop is
  `Emu.Pause()`; `step(n)` resumes with pause-after-n-flips armed, so RPCS3 stops itself at the
  n-th emulated flip. No savestates wired, no controlled start (the frozen stop waits for the boot).
- Buttons: `cross circle square triangle l1 r1 l2 r2 select start l3 r3 up right down left`.
- Memory: `vm` (32-bit guest space, mapped pages only) and `main` (main memory window, batch).
- `get_state` reports the first PPU thread's GPRs, PC, LR and CTR.
- `screenshot` is the flipped display buffer (A8R8G8B8) from RSX local memory.
