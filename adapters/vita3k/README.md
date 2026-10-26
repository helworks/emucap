# emucap: Vita3K (PlayStation Vita) producer

The pinned Vita3K (`upstream.lock`) is built with emucap hooks in its SDL frontend. The producer
from `adapters/_common/emucap_producer` runs inside Vita3K and serves the control wire itself.
`emucap-vita3k-host` is a wrapper that prepares a generation-owned profile and starts Vita3K.

## Build

```bash
adapters/vita3k/build.sh             # EMUCAP_VITA3K_SRC=<recursive checkout> reuses local objects
```

`host/apply-hooks.py` edits the work tree only:

- `display/src/display.cpp`: the vblank thread calls `g_emucap_vblank_hook` once per emulated
  vblank, outside the display locks.
- `ctrl/src/ctrl.cpp`: port 1 buttons are replaced by the held emucap buttons while engaged.
- `sdl-frontend`: the producer starts with the session; `FrameHost::swap_buffers` reads the
  presented OpenGL frame back before each swap.
- `cmake/qt6.cmake`: the Qt minimum is lowered from 6.11 to 6.10 (Ubuntu 26.04's Qt). Upstream
  raised it in a CI overhaul; nothing built here needs 6.11.

## Machine files

`EMUCAP_VITA3K_PREF` names the operator's Vita3K preference directory (the one holding `os0/`,
`vs0/`, `ux0/` ... as Vita3K installs firmware and apps). The wrapper copies it into the launch
generation, so the source is never modified. Nothing is distributed.

## Behaviour

- Needs an X display (Xvfb works); OpenGL renderer.
- `step` counts emulated vblanks. A frozen stop parks the vblank thread and pauses every guest
  kernel thread (`KernelState::pause_threads`). Guest threads run on host threads between
  vblanks, so frame contents are not repeatable; there are no savestates and no controlled start.
- Buttons: `cross circle square triangle select start up right down left l r`, port 0.
- Memory: `user`, the 32-bit guest address space; only allocated pages are readable or writable.
- `get_state` reports the lowest-id kernel thread's registers and the thread count.
- `screenshot` is the last presented frame (960x544 window).
