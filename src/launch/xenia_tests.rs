use super::*;

#[test]
fn xenia_host_admits_its_content_and_has_no_controlled_start() {
    assert_eq!(ADAPTER.adapter, "xenia");
    assert_eq!(ADAPTER.binary_env, "EMUCAP_XENIA_BIN");
    assert!(!ADAPTER.controlled_start);
    let temp = tempfile::tempdir().unwrap();
    for name in ["helengine.xex", "disc.iso"] {
        let content = temp.path().join(name);
        std::fs::write(&content, b"x").unwrap();
        ADAPTER.validate_content(&content).unwrap();
    }
    let other = temp.path().join("game.wuhb");
    std::fs::write(&other, b"x").unwrap();
    assert!(ADAPTER.validate_content(&other).is_err());
}
