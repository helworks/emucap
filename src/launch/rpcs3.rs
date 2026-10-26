//! RPCS3 (PlayStation 3) producer: `emucap-rpcs3-host` from `adapters/rpcs3/build.sh`.
//!
//! The host wrapper runs the emucap-patched RPCS3 with a generation-owned profile and the
//! operator's installed firmware (`EMUCAP_RPCS3_FIRMWARE`). The producer inside RPCS3 is
//! self-paced: a frozen stop is `Emu.Pause()` and `step` uses RPCS3's pause-after-N-flips frame
//! advance. RPCS3 boots before the producer can freeze it, so there is no controlled start.

use super::producer::ProducerAdapter;

pub const ADAPTER: ProducerAdapter = ProducerAdapter {
    adapter: "rpcs3",
    console: "PlayStation 3",
    binary_env: "EMUCAP_RPCS3_BIN",
    binary_name: "emucap-rpcs3-host",
    build_outputs: &["adapters/rpcs3/work/build"],
    content_extensions: &["self", "bin", "elf"],
    accepts_directory: false,
    content_flag: "--rom",
    build_script: "adapters/rpcs3/build.sh",
    controlled_start: false,
};

#[cfg(test)]
#[path = "rpcs3_tests.rs"]
mod tests;
