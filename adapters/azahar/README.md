# emucap: Azahar (Nintendo 3DS) producer

`emucap-azahar-host` is a headless libretro frontend for the pinned Azahar libretro core
(`upstream.lock`, Azahar 2125.1.1). It runs the software renderer, needs no display or GPU, and
serves emucap's control wire from inside its own process through `adapters/_common/emucap_producer`.
One `retro_run()` is one emulated frame; requests run between frames on the emulator thread.

## Build

```bash
adapters/azahar/build.sh             # EMUCAP_AZAHAR_SRC=<recursive checkout> reuses local objects
```

The script checks out the pinned commit with its submodules into `adapters/azahar/work/azahar`,
applies `patches/`, builds `azahar_libretro.so` (`ENABLE_LIBRETRO=ON`, OpenGL and Vulkan off), then
builds `work/build/emucap-azahar-host` and copies the core beside it. The host loads the core from
its own directory, `--core`, or `EMUCAP_AZAHAR_CORE`. `EMUCAP_AZAHAR_BIN` pins another host.

Patches (applied to the work tree only):

- `0001-emucap-fixed-clock-and-ticks.patch`: when `EMUCAP_FIXED_TIME` is set (the host sets it,
  default 946681277), the core boots from that RTC value and a fixed CPU tick origin instead of
  host time and a random tick offset.
- `0002-emucap-debug-exports.patch`: C exports (`emucap_fcram_*`, `emucap_read_vaddr`,
  `emucap_cpu_*`) that the host resolves with `dlsym` for physical FCRAM, process virtual memory
  and ARM11 registers. The libretro memory map alone only publishes VMAs that exist at load time.

## Machine files

None for homebrew (`.3dsx`) and decrypted titles. Encrypted titles need the operator's AES keys in
the generation's `Azahar/sysdata`; none are distributed. The core's user directory is
`./Azahar` under the launch generation (the host's working directory), including the virtual SD
card the title writes to.

## Behaviour

- `step` counts frames; instruction steps and breakpoints are not offered.
- `launch(start_frozen: true)` parks before the first `retro_run()`.
- Buttons: `a b x y l r zl zr start select up down left right`, port 0. HOME (L3 in the core) is
  not offered because the libretro core also uses it to toggle the screen layout. `pointer`
  `{x, y, down}` in screenshot coordinates drives the touch screen.
- `screenshot` is the core's default stacked layout (400x480: top screen above the bottom screen).
- Memory: `fcram` (physical FCRAM, offset-based, batch-readable), `vaddr` (current process
  virtual space, read-only, unmapped bytes fail the read), `vram` (from the libretro memory map).
- `get_state` reports ARM11 `cpu.r0`..`cpu.r14`, `cpu.pc`, `cpu.cpsr` per core (`arm11` is the
  application core).
- Savestates are the core's `retro_serialize` payload plus the held buttons. Serialization drains
  pending HLE async operations first (the core runs its loop for that), so a save can advance
  emulated time slightly when async work is pending.

## License

GPL-2.0-or-later (Azahar is GPLv2+).
