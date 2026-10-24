//! Isolated launcher for the PSoXide PS1 producer (`psoxide-emucap-host`).
//!
//! PSoXide speaks the control wire from inside its own process: the host boots the content,
//! connects to the direct listener, completes producer authentication with the launch-bound
//! front key, and serves every request on its emulator thread between frames. The launcher only
//! resolves the binary, admits the content, and spawns it detached inside the launch generation.

use std::io;
use std::path::{Path, PathBuf};

use super::{find_on_path, is_runnable_file, spawn_detached, LaunchSpec, RuntimeEnv};

/// Content the PSoXide host boots: disc images and side-loaded PSX-EXEs.
pub const CONTENT_EXTENSIONS: &[&str] = &["cue", "ccd", "bin", "iso", "exe"];

pub struct Launch<'a> {
    pub binary: &'a Path,
    pub content: &'a Path,
    pub log_path: &'a Path,
    pub port: u16,
    pub name: Option<&'a str>,
    pub session_token: Option<&'a str>,
    pub runtime: Option<RuntimeEnv<'a>>,
    pub start_frozen: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Launched {
    pub pid: u32,
    pub runtime_home: PathBuf,
}

fn binary_name() -> &'static str {
    if cfg!(windows) {
        "psoxide-emucap-host.exe"
    } else {
        "psoxide-emucap-host"
    }
}

/// `EMUCAP_PSOXIDE_BIN` pins the producer. Otherwise prefer a host installed beside the running
/// emucap binary, then a repo-local build, then `PATH`.
pub fn resolve_binary(repo_root: &Path) -> Option<PathBuf> {
    if let Some(explicit) = std::env::var_os("EMUCAP_PSOXIDE_BIN") {
        let path = PathBuf::from(explicit);
        return is_runnable_file(&path).then_some(path);
    }
    let name = binary_name();
    let mut candidates = Vec::new();
    if let Ok(executable) = std::env::current_exe() {
        if let Some(directory) = executable.parent() {
            candidates.push(directory.join(name));
        }
    }
    candidates.push(repo_root.join("target/release").join(name));
    candidates
        .into_iter()
        .find(|path| is_runnable_file(path))
        .or_else(|| find_on_path(name))
}

pub fn validate_content(content: &Path) -> io::Result<()> {
    if !content.is_file() {
        return Err(io::Error::new(
            io::ErrorKind::NotFound,
            format!("PS1 content not found: {}", content.display()),
        ));
    }
    let supported = content
        .extension()
        .and_then(|value| value.to_str())
        .is_some_and(|value| {
            CONTENT_EXTENSIONS
                .iter()
                .any(|extension| value.eq_ignore_ascii_case(extension))
        });
    if !supported {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "the PSoXide backend boots .cue, .ccd, .bin, .iso, or PSX .exe content",
        ));
    }
    Ok(())
}

pub fn launch_spec(launch: &Launch<'_>, runtime_home: &Path) -> LaunchSpec {
    let mut spec = LaunchSpec::new(launch.binary, launch.log_path)
        .arg("--path")
        .arg(launch.content.to_string_lossy().into_owned())
        .arg("--port")
        .arg(launch.port.to_string())
        .env(
            "EMUCAP_CONTENT",
            launch.content.to_string_lossy().into_owned(),
        )
        // Relative save-state paths resolve inside the launch generation.
        .cwd(runtime_home);
    if launch.start_frozen {
        spec = spec.env("EMUCAP_START_FROZEN", "1");
    }
    if let Some(name) = launch.name {
        spec = spec.env("EMUCAP_NAME", name);
    }
    if let Some(token) = launch.session_token {
        spec = spec.env("EMUCAP_SESSION_TOKEN", token);
    }
    spec.runtime_env(launch.runtime)
}

pub fn launch(launch: &Launch<'_>) -> io::Result<Launched> {
    validate_content(launch.content)?;
    let runtime_home = super::emu_generation_dir("psoxide", launch.port, launch.runtime)?;
    let pid = spawn_detached(&launch_spec(launch, &runtime_home))?;
    Ok(Launched { pid, runtime_home })
}

#[cfg(test)]
#[path = "psoxide_tests.rs"]
mod tests;
