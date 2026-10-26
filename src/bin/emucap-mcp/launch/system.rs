pub(super) fn normalize_system(system: &str) -> Option<&'static str> {
    match system.trim().to_ascii_lowercase().as_str() {
        "snes" | "super-famicom" | "super-nintendo" | "mesen" | "mesen2" => Some("snes"),
        "gamegear" | "gg" | "game-gear" | "sms" | "mastersystem" | "master-system"
        | "sega-mastersystem" => Some("gamegear"),
        "gb" | "gameboy" | "game-boy" | "dmg" => Some("gb"),
        "gbc" | "gbcolor" | "gameboycolor" | "game-boy-color" | "cgb" => Some("gbc"),
        "gba" | "gameboyadvance" | "game-boy-advance" | "agb" => Some("gba"),
        "nes" | "nintendo" | "famicom" | "fc" => Some("nes"),
        "saturn" | "ss" | "sega-saturn" => Some("saturn"),
        "psx" | "ps1" | "playstation" | "playstation1" => Some("psx"),
        "pce" | "pcengine" | "pc-engine" | "pce-cd" | "pc-engine-cd" => Some("pce"),
        "pcfx" | "pc-fx" => Some("pcfx"),
        "md" | "genesis" | "megadrive" | "mega-drive" | "sega-genesis" | "sega-megadrive" => {
            Some("md")
        }
        "wswan" | "ws" | "wsc" | "wonderswan" | "wonderswan-color" | "wonderswancolor"
        | "wonderswan_color" => Some("wswan"),
        "ngp"
        | "ngpc"
        | "neo-geo-pocket"
        | "neogeo-pocket"
        | "neo-geo-pocket-color"
        | "neogeo-pocket-color"
        | "neogeo_pocket"
        | "neogeo_pocket_color" => Some("ngp"),
        "pc98" | "pc-98" | "mame-pc98" | "pc9801" | "pc9821" => Some("pc98"),
        "neogeo_mvs" | "neo-geo-mvs" | "neogeo-mvs" | "mvs" => Some("neogeo_mvs"),
        "neogeo_aes" | "neo-geo-aes" | "neogeo-aes" | "aes" => Some("neogeo_aes"),
        "neogeo_cd" | "neo-geo-cd" | "neogeo-cd" | "ngcd" => Some("neogeo_cd"),
        "n64" | "nintendo64" | "nintendo-64" => Some("n64"),
        "msx" | "msx2+" | "msx2plus" | "openmsx" => Some("msx"),
        "msx1" => Some("msx1"),
        "msx2" => Some("msx2"),
        "msx2p" => Some("msx2p"),
        "dc" | "dreamcast" | "flycast" | "sega-dreamcast" => Some("dc"),
        "nds" | "ds" | "nintendo-ds" | "nintendods" | "desmume" => Some("nds"),
        "psp" | "ppsspp" | "playstation-portable" => Some("psp"),
        "ps2" | "pcsx2" | "playstation2" | "playstation-2" => Some("ps2"),
        "gamecube" | "game-cube" | "gc" | "ngc" | "dolphin-gc" => Some("gamecube"),
        "wii" | "nintendo-wii" | "dolphin-wii" => Some("wii"),
        "xbox" | "original-xbox" | "ogxbox" => Some("xbox"),
        "x360" | "xbox360" | "xbox-360" | "xenia" => Some("x360"),
        "wiiu" | "wii-u" | "cemu" => Some("wiiu"),
        "ps3" | "playstation3" | "playstation-3" | "rpcs3" => Some("ps3"),
        "psvita" | "vita" | "ps-vita" | "playstation-vita" | "vita3k" => Some("psvita"),
        "3ds" | "n3ds" | "nintendo-3ds" | "azahar" | "citra" => Some("3ds"),
        _ => None,
    }
}

pub(super) fn adapter_for_system(system: &str) -> (&'static str, Option<&'static str>) {
    match system {
        "snes" | "gamegear" | "gb" | "gbc" | "gba" | "nes" => ("mesen2", None),
        "saturn" => ("mednafen", Some("ss")),
        "psx" => ("mednafen", Some("psx")),
        "pce" => ("mednafen", Some("pce")),
        "pcfx" => ("mednafen", Some("pcfx")),
        "md" => ("mednafen", Some("md")),
        "wswan" => ("mednafen", Some("wswan")),
        "ngp" => ("mednafen", Some("ngp")),
        "pc98" => ("mame_pc98", None),
        "neogeo_mvs" | "neogeo_aes" | "neogeo_cd" => ("mame_neogeo", None),
        "n64" => ("mupen64plus", None),
        "msx" | "msx1" | "msx2" | "msx2p" => ("openmsx", None),
        "dc" => ("flycast", None),
        "nds" => ("desmume_nds", None),
        "psp" => ("ppsspp", None),
        "ps2" => ("pcsx2", None),
        "gamecube" | "wii" => ("dolphin", None),
        "xbox" => ("xemu", None),
        "x360" => ("xenia", None),
        "wiiu" => ("cemu", None),
        "ps3" => ("rpcs3", None),
        "psvita" => ("vita3k", None),
        "3ds" => ("azahar", None),
        _ => ("", None),
    }
}

pub(super) fn adapter_for_system_and_backend(
    system: &str,
    pc98_backend: Option<crate::args::Pc98BackendArgs>,
    psx_backend: Option<crate::args::PsxBackendArgs>,
) -> (&'static str, Option<&'static str>) {
    if system == "pc98" && pc98_backend == Some(crate::args::Pc98BackendArgs::Np2kai) {
        ("np2kai", None)
    } else if system == "psx" && psx_backend == Some(crate::args::PsxBackendArgs::Psoxide) {
        ("psoxide", None)
    } else {
        adapter_for_system(system)
    }
}

/// Apply the explicit Nintendo DS backend choice to a routed adapter.
pub(super) fn select_nds_backend(
    system: &str,
    nds_backend: Option<crate::args::NdsBackendArgs>,
    routed: (&'static str, Option<&'static str>),
) -> (&'static str, Option<&'static str>) {
    if system == "nds" && nds_backend == Some(crate::args::NdsBackendArgs::Melonds) {
        ("melonds", None)
    } else {
        routed
    }
}

/// Adapters whose host can stop before the first emulated instruction
/// (`launch(..., start_frozen:true)` reports `launch_start.boundary = pre_first_instruction`).
pub(super) fn adapter_supports_start_frozen(adapter: &str) -> bool {
    matches!(
        adapter,
        "mesen2"
            | "mednafen"
            | "np2kai"
            | "xemu"
            | "psoxide"
            | "mupen64plus"
            | "flycast"
            | "dolphin"
    ) || super::producers::supports_start_frozen(adapter)
}

/// Adapters whose preferred launch starts frozen. Every controlled-start adapter does, except the
/// long-established Mesen2 and Mednafen sessions, which keep their free-running default and
/// advertise the frozen start through `start_frozen_contract`.
pub(super) fn adapter_defaults_start_frozen(adapter: &str) -> bool {
    adapter_supports_start_frozen(adapter) && !matches!(adapter, "mesen2" | "mednafen")
}

pub(super) fn adapter_supports_sound(adapter: &str) -> bool {
    matches!(
        adapter,
        "mednafen" | "mame_pc98" | "xemu" | "mesen" | "flycast" | "openmsx" | "dolphin"
    )
}
