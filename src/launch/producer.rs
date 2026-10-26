//! Shared launcher for in-process emucap producers built on `adapters/_common/emucap_producer`.
//!
//! A producer host boots the content itself, connects to the direct listener named by
//! `EMUCAP_PORT`, completes producer authentication with the launch-bound front key from
//! `EMUCAP_SESSION_TOKEN`, and serves every request on its emulator thread between frames. There
//! is no bridge process. Each adapter module describes its host with a [`ProducerAdapter`] and
//! uses these helpers to resolve the binary, admit content, and spawn the host inside the launch
//! generation.

use std::io;
use std::path::{Path, PathBuf};

use super::{find_on_path, is_runnable_file, spawn_detached, LaunchSpec, RuntimeEnv};

/// Static description of one producer host.
#[derive(Debug, Clone, Copy)]
pub struct ProducerAdapter {
    /// Adapter id (`status.adapter`, launch routing).
    pub adapter: &'static str,
    /// Human-readable console name for messages.
    pub console: &'static str,
    /// Environment variable that pins the host binary.
    pub binary_env: &'static str,
    /// Host executable name without the platform suffix.
    pub binary_name: &'static str,
    /// Repo-relative build outputs, preferred in order.
    pub build_outputs: &'static [&'static str],
    /// Accepted content file extensions (lowercase, without the dot).
    pub content_extensions: &'static [&'static str],
    /// Whether a directory is accepted as content (unpacked titles).
    pub accepts_directory: bool,
    /// Command-line flag that carries the content path.
    pub content_flag: &'static str,
    /// The adapter's `build.sh`, repo-relative.
    pub build_script: &'static str,
    /// Whether the host parks before the first emulated instruction for `start_frozen`.
    pub controlled_start: bool,
}

pub struct Launch<'a> {
    pub binary: &'a Path,
    pub content: &'a Path,
    pub log_path: &'a Path,
    pub port: u16,
    pub name: Option<&'a str>,
    pub session_token: Option<&'a str>,
    pub runtime: Option<RuntimeEnv<'a>>,
    pub start_frozen: bool,
    /// Ask the host for a visible window where it has one.
    pub display: bool,
    /// Adapter-specific environment (firmware locations and the like).
    pub extra_env: Vec<(String, String)>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Launched {
    pub pid: u32,
    pub runtime_home: PathBuf,
}

impl ProducerAdapter {
    pub fn executable_name(&self) -> String {
        if cfg!(windows) {
            format!("{}.exe", self.binary_name)
        } else {
            self.binary_name.to_string()
        }
    }

    /// The explicit `binary_env` path is authoritative; otherwise prefer the adapter build
    /// output in the repo, then a host installed beside the running emucap binary, then `PATH`.
    pub fn resolve_binary(&self, repo_root: &Path) -> Option<PathBuf> {
        if let Some(explicit) = std::env::var_os(self.binary_env) {
            let path = PathBuf::from(explicit);
            return is_runnable_file(&path).then_some(path);
        }
        let name = self.executable_name();
        let mut candidates: Vec<PathBuf> = self
            .build_outputs
            .iter()
            .map(|relative| repo_root.join(relative).join(&name))
            .collect();
        if let Ok(executable) = std::env::current_exe() {
            if let Some(directory) = executable.parent() {
                candidates.push(directory.join(&name));
            }
        }
        candidates
            .into_iter()
            .find(|path| is_runnable_file(path))
            .or_else(|| find_on_path(&name))
    }

    pub fn validate_content(&self, content: &Path) -> io::Result<()> {
        if self.accepts_directory && content.is_dir() {
            return Ok(());
        }
        if !content.is_file() {
            return Err(io::Error::new(
                io::ErrorKind::NotFound,
                format!("{} content not found: {}", self.console, content.display()),
            ));
        }
        let supported = content
            .extension()
            .and_then(|value| value.to_str())
            .is_some_and(|value| {
                self.content_extensions
                    .iter()
                    .any(|extension| value.eq_ignore_ascii_case(extension))
            });
        if !supported {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                format!(
                    "the {} adapter boots {} content",
                    self.adapter,
                    self.content_extensions
                        .iter()
                        .map(|extension| format!(".{extension}"))
                        .collect::<Vec<_>>()
                        .join(", ")
                ),
            ));
        }
        Ok(())
    }

    pub fn launch_spec(&self, launch: &Launch<'_>, runtime_home: &Path) -> LaunchSpec {
        let content = launch.content.to_string_lossy().into_owned();
        let mut spec = LaunchSpec::new(launch.binary, launch.log_path)
            .arg(self.content_flag)
            .arg(content.clone());
        if launch.display {
            spec = spec.arg("--display");
        }
        // Relative save-state paths and emulator user data resolve inside the launch generation.
        spec = spec
            .env("EMUCAP_CONTENT", content)
            .env("EMUCAP_PORT", launch.port.to_string())
            .env(
                "EMUCAP_GENERATION_HOME",
                runtime_home.to_string_lossy().into_owned(),
            )
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
        for (key, value) in &launch.extra_env {
            spec = spec.env(key.clone(), value.clone());
        }
        spec.runtime_env(launch.runtime)
    }

    pub fn launch(&self, launch: &Launch<'_>) -> io::Result<Launched> {
        self.validate_content(launch.content)?;
        let runtime_home = super::emu_generation_dir(self.adapter, launch.port, launch.runtime)?;
        let pid = spawn_detached(&self.launch_spec(launch, &runtime_home))?;
        Ok(Launched { pid, runtime_home })
    }
}

#[cfg(test)]
#[path = "producer_tests.rs"]
mod tests;
