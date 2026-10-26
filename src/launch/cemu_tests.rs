use super::*;

#[test]
fn cemu_host_admits_its_content_and_has_no_controlled_start() {
    assert_eq!(ADAPTER.adapter, "cemu");
    assert_eq!(ADAPTER.binary_env, "EMUCAP_CEMU_BIN");
    assert!(!ADAPTER.controlled_start);
    let temp = tempfile::tempdir().unwrap();
    for name in ["game.wuhb", "game.rpx"] {
        let content = temp.path().join(name);
        std::fs::write(&content, b"x").unwrap();
        ADAPTER.validate_content(&content).unwrap();
    }
    let other = temp.path().join("game.iso");
    std::fs::write(&other, b"x").unwrap();
    assert!(ADAPTER.validate_content(&other).is_err());
}
