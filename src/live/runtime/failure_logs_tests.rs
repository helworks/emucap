use super::*;

const PORT: u16 = 47810;

fn write(path: &Path, bytes: &[u8]) {
    fs::create_dir_all(path.parent().unwrap()).unwrap();
    fs::write(path, bytes).unwrap();
}

#[test]
fn abort_moves_generation_logs_to_the_failed_launch_directory() {
    let tmp = tempfile::tempdir().unwrap();
    let store = RuntimeStore::new(tmp.path().join("sessions"));
    let prepared = store.prepare(PORT).unwrap();
    let generation = prepared.generation_dir();
    write(
        &generation.join("flycast.log"),
        b"boot\nFailed to open ELF\n",
    );
    write(
        &generation.join("emulators/flycast/data/flycast.log"),
        b"inner emulator log\n",
    );
    write(&generation.join("content/game.elf"), b"not a log");

    let logs = prepared.abort_preserving_logs().unwrap();

    assert!(
        !generation.exists(),
        "the generation itself is still removed"
    );
    let directory = store.failed_launch_dir(PORT, prepared.launch_id());
    assert_eq!(logs.directory.as_deref(), Some(directory.as_path()));
    let relative: Vec<&str> = logs
        .logs
        .iter()
        .map(|log| log.relative_path.as_str())
        .collect();
    assert_eq!(
        relative,
        ["emulators/flycast/data/flycast.log", "flycast.log"]
    );
    let primary = logs.primary().unwrap();
    assert_eq!(primary.relative_path, "flycast.log");
    assert_eq!(primary.path, directory.join("flycast.log"));
    assert_eq!(primary.tail, "boot\nFailed to open ELF\n");
    assert_eq!(
        fs::read_to_string(&primary.path).unwrap(),
        "boot\nFailed to open ELF\n"
    );
    assert!(!directory.join("content").exists(), "only logs are kept");
}

#[test]
fn annotate_names_the_preserved_log_and_its_tail() {
    let tmp = tempfile::tempdir().unwrap();
    let store = RuntimeStore::new(tmp.path().join("sessions"));
    let prepared = store.prepare(PORT).unwrap();
    write(
        &prepared.generation_dir().join("pcsx2.log"),
        b"Bus error in memReset\n",
    );

    let logs = prepared.abort_preserving_logs().unwrap();
    let mut failure =
        serde_json::json!({"launched": false, "reason": "adapter did not become ready"});
    logs.annotate(&mut failure);

    let expected = store
        .failed_launch_dir(PORT, prepared.launch_id())
        .join("pcsx2.log");
    assert_eq!(failure["emulator_log"], expected.display().to_string());
    assert_eq!(failure["emulator_log_tail"], "Bus error in memReset\n");
    assert_eq!(failure["failure_logs"]["logs"][0]["bytes"], 22);
    assert_eq!(failure["reason"], "adapter did not become ready");
}

#[test]
fn abort_without_logs_leaves_no_failed_launch_directory() {
    let tmp = tempfile::tempdir().unwrap();
    let store = RuntimeStore::new(tmp.path().join("sessions"));
    let prepared = store.prepare(PORT).unwrap();

    let logs = prepared.abort_preserving_logs().unwrap();
    let mut failure = serde_json::json!({"launched": false});
    logs.annotate(&mut failure);

    assert!(logs.is_empty());
    assert!(!store.failed_launches_dir(PORT).exists());
    assert_eq!(failure, serde_json::json!({"launched": false}));
}

#[test]
fn plain_abort_also_preserves_logs() {
    let tmp = tempfile::tempdir().unwrap();
    let store = RuntimeStore::new(tmp.path().join("sessions"));
    let prepared = store.prepare(PORT).unwrap();
    write(&prepared.generation_dir().join("dolphin.log"), b"x\n");

    prepared.abort().unwrap();

    assert!(store
        .failed_launch_dir(PORT, prepared.launch_id())
        .join("dolphin.log")
        .is_file());
}

#[test]
fn failed_launch_retention_keeps_the_newest_directories() {
    let tmp = tempfile::tempdir().unwrap();
    let store = RuntimeStore::new(tmp.path().join("sessions"));
    let mut launch_ids = Vec::new();
    for _ in 0..MAX_RETAINED_FAILED_LAUNCHES + 3 {
        let prepared = store.prepare(PORT).unwrap();
        write(&prepared.generation_dir().join("mupen64plus.log"), b"log\n");
        prepared.abort().unwrap();
        launch_ids.push(prepared.launch_id().to_string());
        // ULIDs created in the same millisecond are not ordered; keep them distinct.
        std::thread::sleep(std::time::Duration::from_millis(2));
    }

    let mut kept: Vec<String> = fs::read_dir(store.failed_launches_dir(PORT))
        .unwrap()
        .map(|entry| entry.unwrap().file_name().into_string().unwrap())
        .collect();
    kept.sort();
    assert_eq!(kept.len(), MAX_RETAINED_FAILED_LAUNCHES);
    assert_eq!(kept, launch_ids[3..].to_vec());
}

#[test]
fn preservation_does_not_follow_symlinks() {
    let tmp = tempfile::tempdir().unwrap();
    let store = RuntimeStore::new(tmp.path().join("sessions"));
    let prepared = store.prepare(PORT).unwrap();
    let outside = tmp.path().join("outside");
    write(&outside.join("secret.log"), b"outside\n");
    #[cfg(unix)]
    std::os::unix::fs::symlink(&outside, prepared.generation_dir().join("linked")).unwrap();
    #[cfg(unix)]
    std::os::unix::fs::symlink(
        outside.join("secret.log"),
        prepared.generation_dir().join("linked.log"),
    )
    .unwrap();

    let logs = prepared.abort_preserving_logs().unwrap();

    assert!(logs.logs.is_empty());
    assert!(outside.join("secret.log").is_file());
}

#[test]
fn tail_starts_at_a_line_boundary_when_truncated() {
    let tmp = tempfile::tempdir().unwrap();
    let path = tmp.path().join("long.log");
    let mut text = String::new();
    for line in 0..2000 {
        text.push_str(&format!("line {line}\n"));
    }
    fs::write(&path, &text).unwrap();

    let tail = read_tail(&path, 64).unwrap();

    assert!(tail.len() <= 64);
    assert!(tail.starts_with("line "));
    assert!(tail.ends_with("line 1999\n"));
    assert!(text.ends_with(&tail));
}
