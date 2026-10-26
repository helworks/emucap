//! Launch legs for in-process producer hosts (`adapters/_common/emucap_producer`).
//!
//! Every producer host boots its content, connects back to this listener with the launch-bound
//! front key, and serves the control wire itself. The legs differ only in the host binary,
//! accepted content and optional machine files, which `emucap::launch::producer::ProducerAdapter`
//! describes.

use super::*;
use emucap::launch::producer::{Launch as ProducerLaunch, ProducerAdapter};

/// The producer adapter registered under `adapter`, if any.
pub(super) fn producer_for(adapter: &str) -> Option<&'static ProducerAdapter> {
    match adapter {
        "melonds" => Some(&emucap::launch::melonds::ADAPTER),
        "azahar" => Some(&emucap::launch::azahar::ADAPTER),
        _ => None,
    }
}

pub(super) fn is_producer(adapter: &str) -> bool {
    producer_for(adapter).is_some()
}

pub(super) fn precondition(adapter: &ProducerAdapter, root: &Path) -> serde_json::Value {
    let source = format!(
        "{} / {} built by {}",
        adapter.binary_env,
        adapter.executable_name(),
        adapter.build_script
    );
    match adapter.resolve_binary(root) {
        Some(path) => serde_json::json!({
            "available": true,
            "path": path.display().to_string(),
            "source": source,
        }),
        None => serde_json::json!({
            "available": false,
            "kind": "binary-not-found",
            "source": source,
        }),
    }
}

pub(super) fn build_required(adapter: &ProducerAdapter) -> serde_json::Value {
    let script = find_repo_root()
        .map(|root| root.join(adapter.build_script).display().to_string())
        .unwrap_or_else(|| adapter.build_script.to_string());
    serde_json::json!(format!(
        "Build {} with {script}, or set {} to its absolute path.",
        adapter.executable_name(),
        adapter.binary_env
    ))
}

/// Machine files a producer needs from the operator, as environment for the host.
fn machine_env(adapter: &ProducerAdapter) -> Vec<(String, String)> {
    let mut env = Vec::new();
    for key in ["EMUCAP_FIRMWARE_DIR"] {
        if let Some(value) = std::env::var_os(key) {
            env.push((key.to_string(), value.to_string_lossy().into_owned()));
        }
    }
    let _ = adapter;
    env
}

pub(super) fn launch(
    adapter: &ProducerAdapter,
    port: u16,
    token: Option<&str>,
    runtime: RuntimeEnv<'_>,
    a: &LaunchArgs,
) -> serde_json::Value {
    let Some(root) = find_repo_root() else {
        return serde_json::json!({ "launched": false, "error": "emucap repo root not found; set EMUCAP_REPO_ROOT" });
    };
    let Some(binary) = adapter.resolve_binary(&root) else {
        return serde_json::json!({
            "launched": false,
            "reason": format!("{} binary not found", adapter.executable_name()),
            "next_action": build_required(adapter),
        });
    };
    let log =
        match super::plan::adapter_log_path(port, runtime, &format!("{}.log", adapter.adapter)) {
            Ok(path) => path,
            Err(error) => return serde_json::json!({"launched": false, "error": error.to_string()}),
        };
    let display = a.display.unwrap_or(false);
    let launch = ProducerLaunch {
        binary: &binary,
        content: Path::new(&a.content_path),
        log_path: &log,
        port,
        name: a.name.as_deref(),
        session_token: token,
        runtime: Some(runtime),
        start_frozen: a.start_frozen,
        display,
        extra_env: machine_env(adapter),
    };
    match adapter.launch(&launch) {
        Ok(launched) => serde_json::json!({
            "launched": true,
            "adapter": adapter.adapter,
            "pid": launched.pid,
            "display": display,
            "port": port,
            "binary": binary.display().to_string(),
            "emucap_home": launched.runtime_home.display().to_string(),
            "log": log.display().to_string(),
            "isolation": format!("{} runs in an emucap-owned launch-generation directory and does not read or change a user emulator profile.", adapter.executable_name()),
            "next_action": "launch returns after the adapter connects",
        }),
        Err(error) => serde_json::json!({ "launched": false, "error": error.to_string() }),
    }
}
