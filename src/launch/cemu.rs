//! Cemu (Wii U) producer: `emucap-cemu-host` from `adapters/cemu/build.sh`.
//!
//! The host wrapper seeds a generation-owned Cemu profile (OpenGL, a GamePad controller profile)
//! and runs the emucap-patched Cemu, whose OpenGL renderer offers a frame boundary at each TV
//! buffer swap. Cemu has no savestates and no emulator-wide pause, so frozen stops are best
//! effort and there is no controlled start.

use super::producer::ProducerAdapter;

pub const ADAPTER: ProducerAdapter = ProducerAdapter {
    adapter: "cemu",
    console: "Wii U",
    binary_env: "EMUCAP_CEMU_BIN",
    binary_name: "emucap-cemu-host",
    build_outputs: &["adapters/cemu/work/build"],
    content_extensions: &["wuhb", "rpx", "wua", "wud", "wux"],
    accepts_directory: false,
    content_flag: "--rom",
    build_script: "adapters/cemu/build.sh",
    controlled_start: false,
};

#[cfg(test)]
#[path = "cemu_tests.rs"]
mod tests;
