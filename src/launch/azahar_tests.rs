use super::*;

#[test]
fn azahar_host_boots_3ds_homebrew_and_decrypted_titles() {
    assert_eq!(ADAPTER.adapter, "azahar");
    assert_eq!(ADAPTER.binary_env, "EMUCAP_AZAHAR_BIN");
    assert!(ADAPTER
        .build_outputs
        .contains(&"adapters/azahar/work/build"));
    let temp = tempfile::tempdir().unwrap();
    for name in ["game.3dsx", "game.cci", "game.3DS"] {
        let content = temp.path().join(name);
        std::fs::write(&content, b"x").unwrap();
        ADAPTER.validate_content(&content).unwrap();
    }
    let other = temp.path().join("game.nds");
    std::fs::write(&other, b"x").unwrap();
    assert!(ADAPTER.validate_content(&other).is_err());
}
