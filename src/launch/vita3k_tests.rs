use super::*;

#[test]
fn vita3k_host_admits_its_content_and_has_no_controlled_start() {
    assert_eq!(ADAPTER.adapter, "vita3k");
    assert_eq!(ADAPTER.binary_env, "EMUCAP_VITA3K_BIN");
    assert!(!ADAPTER.controlled_start);
    let temp = tempfile::tempdir().unwrap();
    for name in ["game.vpk"] {
        let content = temp.path().join(name);
        std::fs::write(&content, b"x").unwrap();
        ADAPTER.validate_content(&content).unwrap();
    }
    let other = temp.path().join("game.iso");
    std::fs::write(&other, b"x").unwrap();
    assert!(ADAPTER.validate_content(&other).is_err());
}
