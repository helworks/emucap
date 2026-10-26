//! Vita3K (PlayStation Vita) producer: `emucap-vita3k-host` from `adapters/vita3k/build.sh`.
//!
//! The host wrapper installs the `.vpk` into a generation-owned copy of the operator's Vita
//! preference tree (`EMUCAP_VITA3K_PREF`) and runs the emucap-patched Vita3K SDL frontend, which
//! serves the control wire from inside the emulator. Frames are emulated vblanks; Vita3K has no
//! savestates and runs guest threads on host threads, so it does not park before the first
//! instruction.

use super::producer::ProducerAdapter;

pub const ADAPTER: ProducerAdapter = ProducerAdapter {
    adapter: "vita3k",
    console: "PlayStation Vita",
    binary_env: "EMUCAP_VITA3K_BIN",
    binary_name: "emucap-vita3k-host",
    build_outputs: &["adapters/vita3k/work/build"],
    content_extensions: &["vpk"],
    accepts_directory: false,
    content_flag: "--rom",
    build_script: "adapters/vita3k/build.sh",
    controlled_start: false,
};

#[cfg(test)]
#[path = "vita3k_tests.rs"]
mod tests;
