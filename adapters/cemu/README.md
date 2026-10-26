# emucap: Cemu (Wii U) producer

The pinned Cemu 2.6 (`upstream.lock`) is built with emucap hooks; the producer from
`adapters/_common/emucap_producer` runs inside Cemu. `emucap-cemu-host` is a wrapper that seeds a
generation-owned profile and starts Cemu with `-g <title>`.

## Build

```bash
adapters/cemu/build.sh               # EMUCAP_CEMU_SRC=<recursive checkout> reuses local objects
```

Without vcpkg, against distro packages. The script builds the pinned fmt 10.2.1 into the work
tree (newer fmt rejects Cemu's format strings), uses `cmake/hidapiConfig.cmake` for the distro
hidapi, compiles with GCC (clang 21 rejects fmt's consteval format strings in Cemu's precompiled
headers) and appends SPIRV-Tools to the link. `host/apply-hooks.py` edits the work tree only:

- `OpenGLRenderer::SwapBuffers`: in a managed launch, reads the TV frame back and calls the
  producer boundary (`emucap_cemu_tv_swap`).
- `vpad.cpp`: channel 0 GamePad buttons come from the producer while it is connected.
- `glFunctions.h`: loads `glReadBuffer`/`glReadPixels`.

## Machine files

None for unencrypted homebrew (`.wuhb`, `.rpx`). Encrypted titles need the operator's `keys.txt`
(`EMUCAP_CEMU_KEYS`); nothing is distributed.

## Behaviour

- Needs an X display (Xvfb works); OpenGL renderer.
- `step` counts TV buffer swaps on the GPU thread. Cemu has no emulator-wide pause: a frozen stop
  parks the GPU thread at the swap and the PPC threads block on GX2 shortly after (best effort).
  No savestates, no controlled start.
- Buttons: `a b x y left right up down zl zr l r plus minus` (Wii U GamePad, port 0).
- Memory: `vm`, the guest effective address space (MEM2 at 0x10000000, MEM1 at 0xF4000000);
  inaccessible ranges fail.
- `screenshot` is the TV output read back before the swap.
