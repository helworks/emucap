//! Azahar (Nintendo 3DS) producer: `emucap-azahar-host` from `adapters/azahar/build.sh`.
//!
//! The host is a headless libretro frontend for the pinned Azahar libretro core, running the
//! software renderer. It loads `azahar_libretro.so` from its own directory (or
//! `EMUCAP_AZAHAR_CORE`), boots homebrew and decrypted titles without system files, and serves
//! the control wire from inside its own process.

use super::producer::ProducerAdapter;

pub const ADAPTER: ProducerAdapter = ProducerAdapter {
    adapter: "azahar",
    console: "Nintendo 3DS",
    binary_env: "EMUCAP_AZAHAR_BIN",
    binary_name: "emucap-azahar-host",
    build_outputs: &["adapters/azahar/work/build"],
    content_extensions: &["3dsx", "3ds", "cci", "cxi", "app", "elf", "axf"],
    accepts_directory: false,
    content_flag: "--rom",
    build_script: "adapters/azahar/build.sh",
};

#[cfg(test)]
#[path = "azahar_tests.rs"]
mod tests;
