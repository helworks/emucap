use super::*;
use crate::test_env::{lock_env, EnvGuard};

const FAKE: ProducerAdapter = ProducerAdapter {
    adapter: "fakehost",
    console: "Fake console",
    binary_env: "EMUCAP_FAKEHOST_BIN_FOR_TESTS",
    binary_name: "emucap-fakehost",
    build_outputs: &["adapters/fakehost/work/build"],
    content_extensions: &["fk", "fkx"],
    accepts_directory: false,
    content_flag: "--rom",
    build_script: "adapters/fakehost/build.sh",
};

#[cfg(unix)]
fn make_executable(path: &Path) {
    use std::os::unix::fs::PermissionsExt;
    let mut perms = std::fs::metadata(path).unwrap().permissions();
    perms.set_mode(perms.mode() | 0o755);
    std::fs::set_permissions(path, perms).unwrap();
}

fn launch_for<'a>(
    binary: &'a Path,
    content: &'a Path,
    log: &'a Path,
    failure: &'a Path,
    start_frozen: bool,
) -> Launch<'a> {
    Launch {
        binary,
        content,
        log_path: log,
        port: 47891,
        name: Some("fake-test"),
        session_token: Some("front-direct-token"),
        runtime: Some(RuntimeEnv {
            launch_id: "launch-01arz3ndektsv4rrffq69g5fav",
            adapter_failure_path: failure,
        }),
        start_frozen,
        display: false,
        extra_env: vec![("EMUCAP_FAKE_FIRMWARE".into(), "/firmware".into())],
    }
}

#[test]
fn launch_spec_passes_content_and_identity_through_the_environment() {
    let temp = tempfile::tempdir().unwrap();
    let binary = temp.path().join("emucap-fakehost");
    let content = temp.path().join("game.fk");
    let log = temp.path().join("fake.log");
    let failure = temp.path().join("failure.json");
    let home = temp.path().join("generation/emulators/fakehost");
    let spec = FAKE.launch_spec(&launch_for(&binary, &content, &log, &failure, false), &home);
    assert_eq!(spec.args, vec!["--rom", content.to_str().unwrap()]);
    assert_eq!(spec.cwd.as_deref(), Some(home.as_path()));
    for expected in [
        ("EMUCAP_CONTENT", content.to_str().unwrap()),
        ("EMUCAP_PORT", "47891"),
        ("EMUCAP_NAME", "fake-test"),
        ("EMUCAP_SESSION_TOKEN", "front-direct-token"),
        ("EMUCAP_LAUNCH_ID", "launch-01arz3ndektsv4rrffq69g5fav"),
        ("EMUCAP_GENERATION_HOME", home.to_str().unwrap()),
        ("EMUCAP_FAKE_FIRMWARE", "/firmware"),
    ] {
        assert!(
            spec.env
                .contains(&(expected.0.to_string(), expected.1.to_string())),
            "missing {expected:?}"
        );
    }
    assert!(!spec.args.iter().any(|arg| arg.contains("front-direct")));
    assert!(!spec.env.iter().any(|(key, _)| key == "EMUCAP_START_FROZEN"));
}

#[test]
fn start_frozen_and_display_are_explicit() {
    let temp = tempfile::tempdir().unwrap();
    let binary = temp.path().join("emucap-fakehost");
    let content = temp.path().join("game.fk");
    let log = temp.path().join("fake.log");
    let failure = temp.path().join("failure.json");
    let mut launch = launch_for(&binary, &content, &log, &failure, true);
    launch.display = true;
    let spec = FAKE.launch_spec(&launch, temp.path());
    assert!(spec
        .env
        .contains(&("EMUCAP_START_FROZEN".to_string(), "1".to_string())));
    assert_eq!(spec.args.last().map(String::as_str), Some("--display"));
}

#[test]
fn content_admission_checks_existence_and_extension() {
    let temp = tempfile::tempdir().unwrap();
    let good = temp.path().join("game.FKX");
    std::fs::write(&good, b"x").unwrap();
    FAKE.validate_content(&good).unwrap();
    let wrong = temp.path().join("game.iso");
    std::fs::write(&wrong, b"x").unwrap();
    let error = FAKE.validate_content(&wrong).unwrap_err();
    assert_eq!(error.kind(), io::ErrorKind::InvalidInput);
    assert!(error.to_string().contains(".fk, .fkx"), "{error}");
    let missing = FAKE
        .validate_content(&temp.path().join("absent.fk"))
        .unwrap_err();
    assert_eq!(missing.kind(), io::ErrorKind::NotFound);
    assert_eq!(
        FAKE.validate_content(temp.path()).unwrap_err().kind(),
        io::ErrorKind::NotFound
    );
}

#[test]
fn directory_content_is_admitted_only_when_declared() {
    let temp = tempfile::tempdir().unwrap();
    let mut adapter = FAKE;
    adapter.accepts_directory = true;
    adapter.validate_content(temp.path()).unwrap();
}

#[cfg(unix)]
#[test]
fn binary_resolution_prefers_the_pin_then_the_repo_build() {
    let _lock = lock_env();
    let temp = tempfile::tempdir().unwrap();
    let built = temp.path().join("adapters/fakehost/work/build");
    std::fs::create_dir_all(&built).unwrap();
    let built_binary = built.join("emucap-fakehost");
    std::fs::write(&built_binary, b"#!/bin/sh\n").unwrap();
    make_executable(&built_binary);
    let _env = EnvGuard::new(&[FAKE.binary_env]);
    std::env::remove_var(FAKE.binary_env);
    assert_eq!(FAKE.resolve_binary(temp.path()), Some(built_binary.clone()));
    let pinned = temp.path().join("pinned-host");
    std::fs::write(&pinned, b"#!/bin/sh\n").unwrap();
    make_executable(&pinned);
    std::env::set_var(FAKE.binary_env, &pinned);
    assert_eq!(FAKE.resolve_binary(temp.path()), Some(pinned.clone()));
    std::env::set_var(FAKE.binary_env, temp.path().join("missing-host"));
    assert_eq!(FAKE.resolve_binary(temp.path()), None);
}
