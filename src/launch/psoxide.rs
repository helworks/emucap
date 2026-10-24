//! Isolated launcher for the PSoXide PS1 producers: the headless `psoxide-emucap-host` and, for
//! `display: true`, the `psoxide-emulator play-session` window.
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

/// CUE track modes the PSoXide disc loader reads: raw 2352-byte sectors, cooked 2048-byte data
/// sectors (PSoXide-emulator 4333df3 and later), and CD-DA audio. Any other mode (`MODE2/2336`,
/// `CDG`, ...) fails at boot. This list is the whole capability check: change it when the PSoXide
/// host's disc loader changes.
pub const ACCEPTED_CUE_TRACK_MODES: &[&str] = &[
    "MODE1/2352",
    "MODE2/2352",
    "MODE1/2048",
    "MODE2/2048",
    "AUDIO",
];

pub struct Launch<'a> {
    pub binary: &'a Path,
    pub content: &'a Path,
    pub log_path: &'a Path,
    pub port: u16,
    pub name: Option<&'a str>,
    pub session_token: Option<&'a str>,
    pub runtime: Option<RuntimeEnv<'a>>,
    pub start_frozen: bool,
    /// Launch the Play-session window (`psoxide-emulator play-session`) instead of the headless
    /// host. `binary` must then be the GUI executable.
    pub display: bool,
    /// Input tape the session records to / replays from. Required with `display`.
    pub play_tape: Option<&'a Path>,
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

fn gui_binary_name() -> &'static str {
    if cfg!(windows) {
        "psoxide-emulator.exe"
    } else {
        "psoxide-emulator"
    }
}

/// Resolve a producer binary: the explicit `override_env` path is authoritative; otherwise prefer
/// one installed beside the running emucap binary, then a repo-local build, then `PATH`.
fn resolve_named(repo_root: &Path, override_env: &str, name: &str) -> Option<PathBuf> {
    if let Some(explicit) = std::env::var_os(override_env) {
        let path = PathBuf::from(explicit);
        return is_runnable_file(&path).then_some(path);
    }
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

/// `EMUCAP_PSOXIDE_BIN` pins the headless producer.
pub fn resolve_binary(repo_root: &Path) -> Option<PathBuf> {
    resolve_named(repo_root, "EMUCAP_PSOXIDE_BIN", binary_name())
}

/// `EMUCAP_PSOXIDE_GUI_BIN` pins the Play-session window binary (`psoxide-emulator`, built with
/// `--features emucap`).
pub fn resolve_gui_binary(repo_root: &Path) -> Option<PathBuf> {
    resolve_named(repo_root, "EMUCAP_PSOXIDE_GUI_BIN", gui_binary_name())
}

/// A PSoXide window is a Play session, and a Play session needs its tape. The tape path is
/// absolute because the producer runs in its own launch-generation directory.
pub fn validate_play_tape(display: bool, play_tape: Option<&Path>) -> io::Result<()> {
    if play_tape.is_some_and(|tape| !tape.is_absolute()) {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "play_tape must be an absolute path",
        ));
    }
    if display && play_tape.is_none() {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "a PSoXide window is a Play session; pass play_tape",
        ));
    }
    Ok(())
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
    let is_cue = content
        .extension()
        .and_then(|value| value.to_str())
        .is_some_and(|value| value.eq_ignore_ascii_case("cue"));
    if is_cue {
        validate_cue_track_modes(content)?;
    }
    Ok(())
}

/// Reject a CUE sheet that declares a track mode outside `ACCEPTED_CUE_TRACK_MODES` before
/// anything is spawned, naming each unreadable track.
fn validate_cue_track_modes(cue: &Path) -> io::Result<()> {
    let unsupported = crate::cue::tracks(cue)?
        .into_iter()
        .filter(|track| {
            !ACCEPTED_CUE_TRACK_MODES
                .iter()
                .any(|mode| track.mode.eq_ignore_ascii_case(mode))
        })
        .map(|track| format!("track {:02} is {}", track.number, track.mode))
        .collect::<Vec<_>>();
    if unsupported.is_empty() {
        return Ok(());
    }
    Err(io::Error::new(
        io::ErrorKind::InvalidInput,
        format!(
            "the PSoXide backend cannot read this disc image ({}): its disc loader accepts CUE track modes {} only. Convert those tracks to one of these modes, or omit psx_backend to use the Mednafen backend.",
            unsupported.join(", "),
            ACCEPTED_CUE_TRACK_MODES.join(", ")
        ),
    ))
}

pub fn launch_spec(launch: &Launch<'_>, runtime_home: &Path) -> LaunchSpec {
    let content = launch.content.to_string_lossy().into_owned();
    let mut spec = LaunchSpec::new(launch.binary, launch.log_path);
    if launch.display {
        // The session defaults to borderless fullscreen with no Esc key, so ask for a window.
        spec = spec
            .arg("play-session")
            .arg("--emucap")
            .arg("--windowed")
            .arg("--disc")
            .arg(content.clone());
    } else {
        spec = spec
            .arg("--path")
            .arg(content.clone())
            .arg("--port")
            .arg(launch.port.to_string());
    }
    if let Some(tape) = launch.play_tape {
        spec = spec.arg("--tape").arg(tape.to_string_lossy().into_owned());
    }
    // Relative save-state paths resolve inside the launch generation.
    // The window has no `--port`; both producers read the endpoint from EMUCAP_PORT.
    spec = spec
        .env("EMUCAP_CONTENT", content)
        .env("EMUCAP_PORT", launch.port.to_string())
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
    validate_play_tape(launch.display, launch.play_tape)?;
    validate_content(launch.content)?;
    let runtime_home = super::emu_generation_dir("psoxide", launch.port, launch.runtime)?;
    let pid = spawn_detached(&launch_spec(launch, &runtime_home))?;
    Ok(Launched { pid, runtime_home })
}

#[cfg(test)]
#[path = "psoxide_tests.rs"]
mod tests;
