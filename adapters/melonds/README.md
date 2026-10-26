# emucap: melonDS (Nintendo DS) producer

`emucap-melonds-host` links the pinned melonDS core (`upstream.lock`, melonDS 1.1) and serves
emucap's control wire from inside its own process through `adapters/_common/emucap_producer`.
There is no bridge process and no GUI. DeSmuME (`adapters/desmume-nds`) stays the default DS
backend; select this one explicitly with `nds_backend: "melonds"` on `launch_plan` and `launch`.

## Build

```bash
adapters/melonds/build.sh            # EMUCAP_MELONDS_SRC=<checkout> reuses local Git objects
```

The script fetches the pinned commit into `adapters/melonds/work/melonDS`, adds the producer as an
extra frontend directory in that work tree (upstream sources are not patched), and builds
`adapters/melonds/work/build/emucap-melonds-host` with the Qt frontend, OpenGL renderer and GDB
stub disabled. Build dependencies: CMake, Ninja, a C++17 compiler, zlib, libslirp, libarchive
(see the melonDS build docs). `EMUCAP_MELONDS_BIN` pins another host binary.

## Machine files

None. The host boots with melonDS FreeBIOS, generated firmware and direct boot. Cartridge saves,
firmware writes and RTC writes stay in memory; a launch generation never writes them to disk.

## Behaviour

- Requests run on the emulator thread between `NDS::RunFrame()` calls. `step` counts frames
  (VBlank start to VBlank start); instruction steps and breakpoints are not offered.
- `launch(start_frozen: true)` parks before the first emulated instruction.
- `set_input` buttons: `a b select start right left up down r l x y`, port 0 only. A `pointer`
  object `{x, y, down}` in screenshot coordinates drives the touch screen (y >= 192).
- `screenshot` stacks the top screen over the bottom screen (256x384 PNG).
- Memory types: `main` (4 MiB), `shared_wram`, `arm7_wram`, `itcm`, `dtcm` (all offset-based and
  batch-readable) and `arm9`, the ARM9 bus view (not batch-readable; reads can touch I/O).
- `get_state` reports `arm9` (default) or `arm7` registers (`cpu.r0`..`cpu.r15`, `cpu.pc`,
  `cpu.cpsr`).
- Savestates are melonDS `DoSavestate` payloads with the host's held input. `load_state`
  restores into a freshly booted machine, and `save_state` re-seats the running machine from the
  bytes it wrote. A machine that already ran keeps per-process state that `DoSavestate` does not
  capture (seen as a one-VBlank-IRQ shift); re-seating makes the continuation after a save, every
  in-process reload and a load in another process the same machine. The presented frame is not
  part of the state: after `load_state`, advance one frame before taking a screenshot.
- The JIT is off for determinism (`--jit` enables it for unmanaged runs).

## Unmanaged use

```bash
emucap-melonds-host --rom game.nds --frames 600 --dump-png out.png
```

Without `EMUCAP_PORT`, `EMUCAP_SESSION_TOKEN` and `EMUCAP_LAUNCH_ID` the host runs freely.

## License

The host links the GPLv3 melonDS core, so the built binary is GPLv3; the emucap sources in this
directory are GPL-2.0-or-later.
