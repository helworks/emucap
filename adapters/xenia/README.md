# emucap: Xenia Canary (Xbox 360) producer

The pinned Xenia Canary (`upstream.lock`, Linux build) is built with emucap hooks; the producer from
`adapters/_common/emucap_producer` runs inside Xenia. `emucap-xenia-host` is a wrapper that points
Xenia's storage, content and cache roots at the launch generation and starts it with the title.

## Build

```bash
adapters/xenia/build.sh              # EMUCAP_XENIA_SRC=<local clone> avoids a fresh GitHub clone
```

Uses `xenia-build.py build --config=release --cc=clang --target=xenia-app` (clang 21 in
`emucap-new-build:v2`). Submodules are fetched shallowly from their upstream URLs.
`host/apply-hooks.py` edits the work tree only:

- `pm4_command_processor_implement.h`: after every guest swap packet (`XE_SWAP`, right after
  `IssueSwap` refreshed the guest output image) the command processor thread calls
  `g_emucap_swap_hook`, the producer boundary.
- `input_system.cc`: while the producer is connected, user 0 reports a connected gamepad whose
  buttons come from the producer; other users and Xenia's own drivers are untouched.
- `app/CMakeLists.txt`: adds the producer sources and zlib to `xenia_canary`.

## Machine files

None for an unencrypted homebrew `.xex`. Retail discs and packages are the operator's own media.

## Behaviour

- Needs an X display (Xvfb works) and a Vulkan device (Mesa lavapipe works). The wrapper selects
  `--gpu=vulkan --apu=nop --hid=nop --discord=false --mute=true`.
- `step` counts guest swaps on the command processor thread. A frozen stop parks that thread at
  the swap and suspends every guest thread (Xenia's debugger pause, without pausing the command
  processor). Guest threads run on host threads between swaps, so frame contents are not
  repeatable across launches.
- No savestates: Xenia's experimental `Emulator::RestoreFromFile` terminates the running title and
  cannot continue it in the same process. No controlled start: the title starts before the
  producer can freeze it.
- Buttons: `a b x y up down left right start back lb rb ls rs guide` (XInput gamepad, user 0).
- Memory: `main` is guest physical memory (512 MiB, batch reads); `vm` is the title's 32-bit
  virtual address space (committed pages only).
- Registers: the main guest thread's `r0`-`r31`, `lr`, `ctr`, thread id and the guest thread count.
- `screenshot` captures the guest output image of the last swap (Vulkan presenter capture).
- Xenia ignores SIGTERM; emucap's stop escalates to SIGKILL.
