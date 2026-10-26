//! melonDS (Nintendo DS) producer: `emucap-melonds-host` from `adapters/melonds/build.sh`.
//!
//! The host links the pinned melonDS core, boots an `.nds` image with FreeBIOS, generated
//! firmware and direct boot, and serves the control wire from inside its own process. It needs
//! no BIOS or firmware files and never reads a melonDS GUI configuration.

use super::producer::ProducerAdapter;

pub const ADAPTER: ProducerAdapter = ProducerAdapter {
    adapter: "melonds",
    console: "Nintendo DS",
    binary_env: "EMUCAP_MELONDS_BIN",
    binary_name: "emucap-melonds-host",
    build_outputs: &["adapters/melonds/work/build"],
    content_extensions: &["nds"],
    accepts_directory: false,
    content_flag: "--rom",
    build_script: "adapters/melonds/build.sh",
};

#[cfg(test)]
#[path = "melonds_tests.rs"]
mod tests;
