use super::*;
use crate::test_env::{lock_env, EnvGuard};

#[test]
fn launch_spec_passes_content_port_and_launch_identity_privately() {
    let temp = tempfile::tempdir().unwrap();
    let binary = temp.path().join(binary_name());
    let content = temp.path().join("game-pong.exe");
    let log = temp.path().join("psoxide.log");
    let runtime_home = temp.path().join("generation/emulators/psoxide");
    let failure = temp.path().join("failure.json");
    let launch = Launch {
        binary: &binary,
        content: &content,
        log_path: &log,
        port: 47890,
        name: Some("psx-test"),
        session_token: Some("front-direct-token"),
        runtime: Some(RuntimeEnv {
            launch_id: "launch-01arz3ndektsv4rrffq69g5fav",
            adapter_failure_path: &failure,
        }),
        start_frozen: false,
    };
    let spec = launch_spec(&launch, &runtime_home);
    assert_eq!(
        spec.args,
        vec!["--path", content.to_str().unwrap(), "--port", "47890"]
    );
    assert_eq!(spec.cwd.as_deref(), Some(runtime_home.as_path()));
    for expected in [
        ("EMUCAP_CONTENT", content.to_str().unwrap()),
        ("EMUCAP_NAME", "psx-test"),
        ("EMUCAP_SESSION_TOKEN", "front-direct-token"),
        ("EMUCAP_LAUNCH_ID", "launch-01arz3ndektsv4rrffq69g5fav"),
    ] {
        assert!(
            spec.env
                .contains(&(expected.0.to_string(), expected.1.to_string())),
            "missing {expected:?}"
        );
    }
    // The front key travels only through the environment, never the command line.
    assert!(!spec.args.iter().any(|arg| arg.contains("front-direct")));
    assert!(!spec.env.iter().any(|(key, _)| key == "EMUCAP_START_FROZEN"));
}

#[test]
fn start_frozen_requests_the_pre_first_instruction_boundary() {
    let temp = tempfile::tempdir().unwrap();
    let path = temp.path().join("x");
    let launch = Launch {
        binary: &path,
        content: &path,
        log_path: &path,
        port: 47891,
        name: None,
        session_token: None,
        runtime: None,
        start_frozen: true,
    };
    let spec = launch_spec(&launch, temp.path());
    assert!(spec
        .env
        .contains(&("EMUCAP_START_FROZEN".into(), "1".into())));
    assert!(!spec.env.iter().any(|(key, _)| key == "EMUCAP_NAME"));
}

#[test]
fn content_validation_accepts_disc_images_and_psx_executables_only() {
    let temp = tempfile::tempdir().unwrap();
    for extension in ["ccd", "bin", "iso", "exe", "EXE"] {
        let path = temp.path().join(format!("game.{extension}"));
        std::fs::write(&path, b"content").unwrap();
        assert!(validate_content(&path).is_ok(), "{extension}");
    }
    let cue = temp.path().join("game.cue");
    std::fs::write(
        &cue,
        "FILE \"game.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n",
    )
    .unwrap();
    assert!(validate_content(&cue).is_ok());
    for extension in ["chd", "pbp", "zip"] {
        let path = temp.path().join(format!("game.{extension}"));
        std::fs::write(&path, b"content").unwrap();
        assert!(validate_content(&path).is_err(), "{extension}");
    }
    assert!(validate_content(&temp.path().join("missing.cue")).is_err());
}

#[test]
fn cue_validation_admits_only_track_modes_the_disc_loader_reads() {
    let temp = tempfile::tempdir().unwrap();
    let cue = temp.path().join("disc.cue");
    let write = |tracks: &str| {
        std::fs::write(&cue, format!("FILE \"disc.bin\" BINARY\n{tracks}")).unwrap();
    };
    write(
        "  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n  TRACK 02 AUDIO\n    INDEX 01 00:02:00\n",
    );
    assert!(validate_content(&cue).is_ok());
    write("  TRACK 01 mode1/2352\n    INDEX 01 00:00:00\n");
    assert!(validate_content(&cue).is_ok());
    // Cooked 2048-byte data tracks are readable too.
    write("  TRACK 01 MODE2/2048\n    INDEX 01 00:00:00\n  TRACK 02 MODE1/2048\n    INDEX 01 00:02:00\n");
    assert!(validate_content(&cue).is_ok());

    // Other modes, alone or after a readable track: every unreadable track is named.
    for (tracks, named) in [
        ("  TRACK 01 MODE2/2336\n    INDEX 01 00:00:00\n", vec!["track 01 is MODE2/2336"]),
        (
            "  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n  TRACK 02 CDG\n    INDEX 01 00:02:00\n  TRACK 03 MODE2/2336\n    INDEX 01 00:04:00\n",
            vec!["track 02 is CDG", "track 03 is MODE2/2336"],
        ),
    ] {
        write(tracks);
        let error = validate_content(&cue).unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::InvalidInput);
        let message = error.to_string();
        for track in named {
            assert!(message.contains(track), "{message}");
        }
        assert!(!message.contains("track 01 is MODE2/2352"), "{message}");
    }
}

#[test]
fn every_accepted_cue_track_mode_is_one_the_cue_parser_recognises() {
    let temp = tempfile::tempdir().unwrap();
    let cue = temp.path().join("disc.cue");
    for mode in ACCEPTED_CUE_TRACK_MODES {
        std::fs::write(
            &cue,
            format!("FILE \"disc.bin\" BINARY\n  TRACK 01 {mode}\n    INDEX 01 00:00:00\n"),
        )
        .unwrap();
        assert!(validate_content(&cue).is_ok(), "{mode}");
    }
}

#[test]
fn explicit_binary_override_is_authoritative() {
    let _lock = lock_env();
    let _guard = EnvGuard::new(&["EMUCAP_PSOXIDE_BIN"]);
    let temp = tempfile::tempdir().unwrap();
    std::env::set_var("EMUCAP_PSOXIDE_BIN", temp.path().join("missing-host"));
    assert_eq!(resolve_binary(temp.path()), None);
}
