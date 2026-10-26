//! Keep the emulator logs of a launch generation that is being aborted.
//!
//! A failed launch removes its generation directory. The emulator log inside it is usually the
//! only record of why the guest never reached adapter readiness (a missing BIOS, a loader that
//! rejects the content, a SIGBUS at VM start), so the logs move to a stable per-port
//! `failed-launches/<launch_id>/` directory first and the launch failure names them.

use super::*;

/// Directory under a port's session directory that keeps the logs of failed launches.
pub const FAILED_LAUNCHES_DIR: &str = "failed-launches";
/// Number of failed launches whose logs are retained per port; older ones are pruned.
pub const MAX_RETAINED_FAILED_LAUNCHES: usize = 8;
/// Bytes of each preserved log returned inline as its tail.
pub const FAILURE_LOG_TAIL_BYTES: u64 = 4096;
const MAX_PRESERVED_LOG_FILES: usize = 32;
const MAX_SCAN_DEPTH: usize = 8;
const MAX_SCANNED_ENTRIES: usize = 20_000;

/// One log file moved out of an aborted generation.
#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct PreservedLog {
    /// Absolute path of the preserved copy.
    pub path: PathBuf,
    /// Path relative to the generation directory it came from.
    pub relative_path: String,
    pub bytes: u64,
    /// The last `FAILURE_LOG_TAIL_BYTES` of the log, lossily decoded, starting at a line boundary.
    pub tail: String,
}

/// What an aborted generation left behind for diagnosis.
#[derive(Debug, Clone, Default, Serialize, PartialEq, Eq)]
pub struct FailureLogs {
    #[serde(skip_serializing_if = "Option::is_none")]
    pub directory: Option<PathBuf>,
    pub logs: Vec<PreservedLog>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub error: Option<String>,
}

impl FailureLogs {
    pub fn is_empty(&self) -> bool {
        self.logs.is_empty() && self.error.is_none()
    }

    /// The launcher log at the generation root (`<adapter>.log`), else the first preserved log.
    pub fn primary(&self) -> Option<&PreservedLog> {
        self.logs
            .iter()
            .find(|log| !log.relative_path.contains('/') && log.relative_path.ends_with(".log"))
            .or_else(|| self.logs.first())
    }

    /// Add `failure_logs`, `emulator_log` and `emulator_log_tail` to a launch failure object.
    pub fn annotate(&self, failure: &mut serde_json::Value) {
        if self.is_empty() {
            return;
        }
        let Some(object) = failure.as_object_mut() else {
            return;
        };
        if let Ok(value) = serde_json::to_value(self) {
            object.insert("failure_logs".into(), value);
        }
        if let Some(primary) = self.primary() {
            object.insert(
                "emulator_log".into(),
                serde_json::json!(primary.path.display().to_string()),
            );
            object.insert(
                "emulator_log_tail".into(),
                serde_json::json!(primary.tail.clone()),
            );
        }
    }
}

impl RuntimeStore {
    pub fn failed_launches_dir(&self, port: u16) -> PathBuf {
        self.session_dir(port).join(FAILED_LAUNCHES_DIR)
    }

    pub fn failed_launch_dir(&self, port: u16, launch_id: &str) -> PathBuf {
        self.failed_launches_dir(port).join(launch_id)
    }

    /// Move the logs of `launch_id`'s generation into its failed-launch directory.
    pub(super) fn preserve_failure_logs(&self, port: u16, launch_id: &str) -> FailureLogs {
        match self.try_preserve_failure_logs(port, launch_id) {
            Ok(logs) => logs,
            Err(error) => FailureLogs {
                error: Some(format!("failed to preserve emulator logs: {error}")),
                ..FailureLogs::default()
            },
        }
    }

    fn try_preserve_failure_logs(&self, port: u16, launch_id: &str) -> io::Result<FailureLogs> {
        validate_launch_id(launch_id)?;
        let generation = self.generation_dir(port, launch_id);
        let candidates = collect_log_files(&generation)?;
        if candidates.is_empty() {
            return Ok(FailureLogs::default());
        }
        let destination = self.failed_launch_dir(port, launch_id);
        self.create_managed_dir(&destination)?;
        let mut logs = Vec::with_capacity(candidates.len());
        for relative in candidates {
            let source = generation.join(&relative);
            let target = destination.join(&relative);
            if let Some(parent) = target.parent() {
                self.create_managed_dir(parent)?;
            }
            if fs::rename(&source, &target).is_err() {
                fs::copy(&source, &target)?;
            }
            let bytes = fs::metadata(&target)?.len();
            logs.push(PreservedLog {
                tail: read_tail(&target, FAILURE_LOG_TAIL_BYTES)?,
                path: target,
                relative_path: relative_display(&relative),
                bytes,
            });
        }
        let _ = self.prune_failed_launches(port, launch_id);
        Ok(FailureLogs {
            directory: Some(destination),
            logs,
            error: None,
        })
    }

    /// Keep the newest `MAX_RETAINED_FAILED_LAUNCHES` failed-launch directories (launch ids are
    /// ULIDs, so their names sort by creation time). `keep` is never removed.
    fn prune_failed_launches(&self, port: u16, keep: &str) -> io::Result<()> {
        let root = self.failed_launches_dir(port);
        let mut names: Vec<String> = fs::read_dir(&root)?
            .filter_map(Result::ok)
            .filter_map(|entry| entry.file_name().into_string().ok())
            .filter(|name| validate_launch_id(name).is_ok())
            .collect();
        names.sort();
        let excess = names.len().saturating_sub(MAX_RETAINED_FAILED_LAUNCHES);
        for name in names.into_iter().take(excess) {
            if name != keep {
                remove_path(&root.join(name))?;
            }
        }
        Ok(())
    }
}

fn is_log_file(name: &str) -> bool {
    name.ends_with(".log") || name == "adapter-failure.json"
}

/// Relative paths of log files under `root`, without following symlinks, sorted.
fn collect_log_files(root: &Path) -> io::Result<Vec<PathBuf>> {
    let mut found = Vec::new();
    let mut scanned = 0usize;
    let mut pending = vec![(PathBuf::new(), 0usize)];
    while let Some((relative, depth)) = pending.pop() {
        let entries = match fs::read_dir(root.join(&relative)) {
            Ok(entries) => entries,
            Err(error) if error.kind() == io::ErrorKind::NotFound => continue,
            Err(error) => return Err(error),
        };
        for entry in entries {
            let entry = entry?;
            scanned += 1;
            if scanned > MAX_SCANNED_ENTRIES {
                break;
            }
            let file_type = entry.file_type()?;
            let child = relative.join(entry.file_name());
            if file_type.is_dir() {
                if depth + 1 < MAX_SCAN_DEPTH {
                    pending.push((child, depth + 1));
                }
            } else if file_type.is_file() && entry.file_name().to_str().is_some_and(is_log_file) {
                found.push(child);
            }
        }
    }
    found.sort();
    found.truncate(MAX_PRESERVED_LOG_FILES);
    Ok(found)
}

fn relative_display(relative: &Path) -> String {
    relative
        .components()
        .map(|component| component.as_os_str().to_string_lossy())
        .collect::<Vec<_>>()
        .join("/")
}

/// The last `limit` bytes of `path`, starting after the first line break when truncated.
pub(crate) fn read_tail(path: &Path, limit: u64) -> io::Result<String> {
    use std::io::{Seek, SeekFrom};
    let mut file = File::open(path)?;
    let length = file.metadata()?.len();
    let start = length.saturating_sub(limit);
    file.seek(SeekFrom::Start(start))?;
    let mut bytes = Vec::with_capacity((length - start) as usize);
    file.take(limit).read_to_end(&mut bytes)?;
    let text = String::from_utf8_lossy(&bytes).into_owned();
    if start == 0 {
        return Ok(text);
    }
    Ok(match text.find('\n') {
        Some(index) if index + 1 < text.len() => text[index + 1..].to_string(),
        _ => text,
    })
}

#[cfg(test)]
#[path = "failure_logs_tests.rs"]
mod tests;
