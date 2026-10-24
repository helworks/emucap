use std::path::Path;

pub(crate) use emucap::content_identity::normalize_rom_sha1;

/// Return the identity captured before a managed launch. This read-only status path never expands
/// an adapter-reported descriptor path into indirect filesystem reads.
pub(crate) fn content_identity_for_rom_info(
    value: &serde_json::Value,
    endpoint_port: Option<u16>,
    live_launch_id: Option<&str>,
) -> std::io::Result<Option<emucap::content_identity::ContentIdentity>> {
    content_identity_for_rom_info_with_store(
        value,
        endpoint_port,
        live_launch_id,
        &emucap::live::runtime::RuntimeStore::discover()?,
    )
}

pub(crate) fn content_identity_for_rom_info_with_store(
    value: &serde_json::Value,
    endpoint_port: Option<u16>,
    live_launch_id: Option<&str>,
    store: &emucap::live::runtime::RuntimeStore,
) -> std::io::Result<Option<emucap::content_identity::ContentIdentity>> {
    let observed_path = value.get("path").and_then(serde_json::Value::as_str);
    let observed_adapter = value.get("adapter").and_then(serde_json::Value::as_str);
    if let (Some(port), Some(live_launch_id)) = (endpoint_port, live_launch_id) {
        if let Some(current) = store.read_current(port)? {
            if current.launch_id == live_launch_id {
                if let Some(bound) = current.content_identity {
                    if observed_path.is_some_and(|observed| {
                        !emucap::content_identity::same_content_path(&current.content, observed)
                    }) {
                        return Err(std::io::Error::new(
                            std::io::ErrorKind::InvalidData,
                            "managed adapter content path does not match its launch generation",
                        ));
                    }
                    return Ok(Some(bound));
                }
            }
        }
    }
    let descriptor = observed_path
        .and_then(|path| Path::new(path).extension())
        .and_then(|extension| extension.to_str())
        .is_some_and(|extension| {
            matches!(
                extension.to_ascii_lowercase().as_str(),
                "cue" | "gdi" | "ccd" | "toc" | "m3u"
            )
        });
    if descriptor {
        return Err(std::io::Error::new(
            std::io::ErrorKind::PermissionDenied,
            format!(
                "unmanaged {} descriptor identity is unavailable without launch-time member approval",
                observed_adapter.unwrap_or("emulator")
            ),
        ));
    }
    Ok(None)
}
