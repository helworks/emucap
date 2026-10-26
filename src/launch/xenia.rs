//! Xenia Canary (Xbox 360) producer: `emucap-xenia-host` from `adapters/xenia/build.sh`.
//!
//! The host wrapper runs the emucap-patched Xenia Canary (Vulkan renderer, nop audio and input
//! backends) with its storage, content and cache roots inside the launch generation. The producer
//! inside Xenia offers a frame boundary after every guest swap on the command processor thread and
//! suspends guest threads at frozen stops. Xenia runs guest threads on host threads, its
//! savestates cannot restore into a running title, and the title starts before the producer can
//! freeze it, so there are no savestates and no controlled start.

use super::producer::ProducerAdapter;

pub const ADAPTER: ProducerAdapter = ProducerAdapter {
    adapter: "xenia",
    console: "Xbox 360",
    binary_env: "EMUCAP_XENIA_BIN",
    binary_name: "emucap-xenia-host",
    build_outputs: &["adapters/xenia/work/build"],
    content_extensions: &["xex", "iso"],
    accepts_directory: false,
    content_flag: "--rom",
    build_script: "adapters/xenia/build.sh",
    controlled_start: false,
};

#[cfg(test)]
#[path = "xenia_tests.rs"]
mod tests;
