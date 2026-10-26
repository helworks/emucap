use super::*;
use std::path::Path;

#[test]
fn melonds_host_boots_nds_images_from_its_build_output() {
    assert_eq!(ADAPTER.adapter, "melonds");
    assert_eq!(ADAPTER.content_flag, "--rom");
    assert_eq!(ADAPTER.binary_env, "EMUCAP_MELONDS_BIN");
    assert!(ADAPTER
        .build_outputs
        .contains(&"adapters/melonds/work/build"));
    let temp = tempfile::tempdir().unwrap();
    let rom = temp.path().join("game.nds");
    std::fs::write(&rom, b"rom").unwrap();
    ADAPTER.validate_content(&rom).unwrap();
    let other = temp.path().join("game.3dsx");
    std::fs::write(&other, b"rom").unwrap();
    assert!(ADAPTER.validate_content(&other).is_err());
    assert!(ADAPTER
        .validate_content(Path::new("/nonexistent/game.nds"))
        .is_err());
}
