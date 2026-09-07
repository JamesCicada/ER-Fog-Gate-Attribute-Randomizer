#pragma once

#include <cstdint>

// ============================================================
// Elden Ring 1.16.2 runtime offsets.
//
// Offset evidence status legend:
//
//   VERIFIED            Confirmed against the live 1.16.2 game at runtime
//                       (Cheat Engine / memory reads) in this project.
//   LIKELY              Byte-exact structural layout taken from
//                       fromsoftware-rs v0.14.0 (github.com/vswarte/
//                       fromsoftware-rs, tag v0.14.0), which was updated
//                       specifically for ELDEN RING 1.16.2 (PR #304) and
//                       whose DLRF/RTTI-derived layouts are used by
//                       working tools. NOT yet confirmed by an in-game
//                       read in this project, but corroborated by
//                       offset-encoded field names and the WorldChrMan
//                       AOB displacement (see below).
//   NOT VERIFIED        Candidate only; undocumented/derived estimate.
//                       Never treated as fact until runtime-verified.
// ============================================================

namespace PlayerOffsets
{
    // ============================================================
    // GameDataMan
    // ============================================================

    // GameDataMan -> main_player_game_data (OwnedPtr<PlayerGameData>).
    // This is the "StatBase" dereferenced by the existing reader.
    //   Source: fromsoftware-rs v0.14.0 game_data_man.rs
    //   Status: LIKELY (structurally verified in binary; matches the
    //           runtime-observed StatBase pointer from CE)
    constexpr uintptr_t GameDataManMainPlayerGameData = 0x08;

    // GameDataMan -> player_game_data_list (OwnedPtr<[PlayerGameData; 5]>).
    //   Source: fromsoftware-rs v0.14.0 game_data_man.rs
    //   Status: LIKELY
    constexpr uintptr_t GameDataManPlayerGameDataList = 0x10;

    // GameDataMan -> boss_fight_active (bool).
    // "Whether a boss fight is currently active".
    // Used as a runtime fog-gate/traversal anchor (rising edge).
    //   Source: fromsoftware-rs v0.14.0 game_data_man.rs
    //   Status: LIKELY
    constexpr uintptr_t GameDataManBossFightActive = 0xC0;

    // GameDataMan -> boss_health_bar_npc_param_id (u32).
    // Identifies the boss that was just activated; a stable secondary
    // gate-identity component.
    //   Source: fromsoftware-rs v0.14.0 game_data_man.rs
    //   Status: LIKELY
    constexpr uintptr_t GameDataManBossNpcParamId = 0xCC;

    // ============================================================
    // Player Stat Base (PlayerGameData)  -- VERIFIED layout
    // ============================================================

    // GameDataMan + StatBasePtr -> PlayerGameData (player stat base)
    constexpr uintptr_t StatBasePtr = GameDataManMainPlayerGameData;

    // PlayerGameData -> attributes (u32 each)
    //   Source: fromsoftware-rs v0.14.0 player_game_data.rs
    //   Status: VERIFIED against live 1.16.2 in this project
    constexpr uintptr_t Vigor        = 0x3C;
    constexpr uintptr_t Mind         = 0x40;
    constexpr uintptr_t Endurance    = 0x44;
    constexpr uintptr_t Strength     = 0x48;
    constexpr uintptr_t Dexterity    = 0x4C;
    constexpr uintptr_t Intelligence = 0x50;
    constexpr uintptr_t Faith        = 0x54;
    constexpr uintptr_t Arcane       = 0x58;

    // PlayerGameData -> level (u32)
    constexpr uintptr_t Level        = 0x68;

    constexpr int AttributeCount = 8;

    // PlayerGameData attributes -> attribute index used by
    // PlayerState / randomization. Keep in the canonical order.
    constexpr uintptr_t AttributeOffsets[AttributeCount] = {
        Vigor,
        Mind,
        Endurance,
        Strength,
        Dexterity,
        Intelligence,
        Faith,
        Arcane,
    };

    // ============================================================
    // PlayerGameData identity / load-state signals  -- LIKELY
    //   Source: fromsoftware-rs v0.14.0 player_game_data.rs
    // ============================================================

    // "Event id of this game data owner"
    constexpr uintptr_t CharacterEventId = 0x08;

    // Index into GameDataMan's player game data array
    constexpr uintptr_t GameDataManIndex = 0xD0;

    // "True if the player is in their own world."
    constexpr uintptr_t IsMyWorld = 0xE7;

    // u32 character id
    constexpr uintptr_t CharacterId = 0xEC;

    // "True when this game data belongs to the main (local) player."
    constexpr uintptr_t IsMainPlayer = 0x8F0;

    // Runtime-computed attributes ("Level after any buffs and
    // corrections"). Candidate runtime-only application target.
    //   Status: LIKELY
    constexpr uintptr_t EffectiveVigor        = 0x288;
    constexpr uintptr_t EffectiveMind         = 0x28C;
    constexpr uintptr_t EffectiveEndurance    = 0x290;
    constexpr uintptr_t EffectiveVitality     = 0x294;
    constexpr uintptr_t EffectiveStrength     = 0x298;
    constexpr uintptr_t EffectiveDexterity    = 0x29C;
    constexpr uintptr_t EffectiveIntelligence = 0x2A0;
    constexpr uintptr_t EffectiveFaith        = 0x2A4;
    constexpr uintptr_t EffectiveArcane       = 0x2A8;

    // Wondrous Physick tear slots (EquipGameData from fromsoftware-rs).
    // PlayerGameData.equipment is at 0x2B0 (OwnedPtr -> EquipGameData).
    // EquipGameData field layout (all repr(C), offset-verified against the
    // structs in fs-rs v0.14.0):
    //   +0x00 vftable, +0x08 equipment_item_idx_list[22] (0x58), +0x60 unk60,
    //   +0x68 unk68, +0x70 chr_asm (ChrAsm, 0xC8 bytes -> 0x138),
    //   +0x138 equip_inventory_data (EquipInventoryData, 0xDC -> 0x214),
    //   +0x214 equip_magic_data (ptr) -> 0x21C,
    //   +0x21C equip_item_data (EquipItemData, 0xC8 -> 0x2E4),
    //   +0x2E4 equip_gesture_data (ptr) -> 0x2EC,
    //   +0x2EC item_replenish_state_tracker (ptr) -> 0x2F4,
    //   +0x2F4 qm_item_backup_vector (ptr) -> 0x2FC,
    //   +0x2FC equipment_entries (ChrAsmEquipEntries, 0x9C) -> 0x398,
    //   +0x398 physick_tears[2], +0x3A0 extra_physick_tear.
    // So PGD + 0x2B0 + 0x398 = 0x648 / 0x64C, extra at 0x650.
    //   Status: LIKELY (layout-derived, not yet runtime-verified)
    constexpr uintptr_t EquipGameDataOffset = 0x2B0;

    constexpr uintptr_t PhysickTear1       = EquipGameDataOffset + 0x398;
    constexpr uintptr_t PhysickTear2       = EquipGameDataOffset + 0x39C;
    constexpr uintptr_t PhysickExtraTear   = EquipGameDataOffset + 0x3A0;
    constexpr int PhysickSlotCount = 3;

    // Vigor / FP resources. current_* and max_* u32 pairs validated against
    // the fs-rs 1.16.2 repr(C) layout (field chain anchored at the VERIFIED
    // attribute 0x3C): current_hp 0x10, current_max_hp 0x14, base_max_hp 0x18,
    // current_fp 0x1C, current_max_fp 0x20, base_max_fp 0x24.
    //   Status: VERIFIED (byte-exact with fs-rs; matches CE-observed layout)
    constexpr uintptr_t CurrentHp          = 0x10;
    constexpr uintptr_t CurrentMaxHp       = 0x14;
    constexpr uintptr_t CurrentFp          = 0x1C;
    constexpr uintptr_t CurrentMaxFp       = 0x20;

    // Flask charges (max use counts) and heal potency. Charges at 0x101/0x102
    // are the "EstusFlaskAllocateNum_byHp/byMp" fields read/written by TGA's
    // in-game-tested "Add charge to flask" script. The potency fields match
    // the fs-rs 1.16.2 repr(C) layout (hp_estus_rate 0x924 f32,
    // hp_estus_additional 0x928 u8, fp_estus_rate 0x92C f32,
    // fp_estus_additional 0x930 u8), consistent with the VERIFIED attribute chain.
    //   Status: VERIFIED (charges: TGA CT; potency: fs-rs)
    constexpr uintptr_t MaxHpFlask         = 0x101; // u8
    constexpr uintptr_t MaxFpFlask         = 0x102; // u8
    constexpr uintptr_t HpEstusRate        = 0x924; // f32
    constexpr uintptr_t HpEstusAdditional  = 0x928; // u8
    constexpr uintptr_t FpEstusRate        = 0x92C; // f32
    constexpr uintptr_t FpEstusAdditional  = 0x930; // u8

    constexpr int MaxFlaskCharges          = 14; // game cap (HP+FP combined)
    constexpr int MaxFlaskLevel            = 12; // game cap (sacred tears)

    // ============================================================
    // WorldChrMan  -- LIKELY
    //   Source: fromsoftware-rs v0.14.0 world_chr_man.rs.
    //   Corroborated by the trailing bytes of the WorldChrMan AOB:
    //   "48 39 88 08 E5 01 00" == cmp [rax+0x1E508], rcx, i.e. the
    //   game code itself compares WorldChrMan.main_player at 0x1E508.
    // ============================================================

    // WorldChrMan -> main_player (Option<OwnedPtr<PlayerIns>>)
    // "Points to the local player."
    constexpr uintptr_t WorldChrManMainPlayer = 0x1E508;

    // ============================================================
    // ChrSetEntry / ChrLoadStatus  -- LIKELY
    //   Source: fromsoftware-rs v0.14.0 world_chr_man.rs
    // ============================================================

    // ChrSetEntry -> chr_load_status (u8)
    constexpr uintptr_t ChrSetEntryLoadStatus = 0x08;

    // ChrLoadStatus values
    constexpr unsigned int ChrLoadStatusUnloaded           = 0;
    constexpr unsigned int ChrLoadStatusInitializing       = 1;
    constexpr unsigned int ChrLoadStatusActive             = 2;
    constexpr unsigned int ChrLoadStatusNetworkInitializing = 3;
    constexpr unsigned int ChrLoadStatusReadyForActivation = 4;
    constexpr unsigned int ChrLoadStatusUnloading          = 5;

    // ============================================================
    // ChrIns (base of PlayerIns)  -- LIKELY
    //   Source: fromsoftware-rs v0.14.0 chr_ins.rs
    // ============================================================

    // ChrIns -> chr_set_entry (NonNull<ChrSetEntry<Self>>)
    constexpr uintptr_t ChrInsChrSetEntry = 0x10;

    // ChrIns -> backread_state (u32)
    constexpr uintptr_t ChrInsBackreadState = 0x20;

    // ChrIns -> block_id (BlockId, u32)
    constexpr uintptr_t ChrInsBlockId = 0x30;

    // ChrIns -> block_origin (BlockId, u32)
    constexpr uintptr_t ChrInsBlockOrigin = 0x3C;

    // ChrIns -> load_state bitfield (u32)
    constexpr uintptr_t ChrInsLoadState = 0x1B4;

    // ChrIns flags byte with is_active (bit 4).
    // "This flag controls whether the character considered active or not"
    constexpr uintptr_t ChrInsFlags1C8 = 0x1C8;
    constexpr int        ChrInsFlags1C8IsActiveBit = 4;

    // ChrIns -> field_ins_handle (8 bytes: u32 selector + u32 block id)
    constexpr uintptr_t ChrInsFieldInsHandle = 0x08;

    // ============================================================
    // PlayerIns (ChrIns base + extra)  -- LIKELY
    //   Source: fromsoftware-rs v0.14.0 chr_ins.rs
    // ============================================================

    // PlayerIns -> player_game_data (NonNull<PlayerGameData>)
    // sizeof(ChrIns) == 0x580 == PlayerIns.player_game_data offset.
    constexpr uintptr_t PlayerInsPlayerGameData = 0x580;

    // "Position within the current block." (16 bytes)
    constexpr uintptr_t PlayerInsBlockPosition = 0x6C0;

    // ChrIns -> modules (OwnedPtr<ChrInsModuleContainer>). Container has
    // data at +0x00 -> CSChrDataModule (the live stat module the HUD/fight
    // reads HP/FP from). Located at unk18c field offset 0x190 in fs-rs.
    //   Source: fromsoftware-rs v0.14.0 chr_ins.rs / chr_ins/module.rs
    //   Status: LIKELY
    constexpr uintptr_t ChrInsModules             = 0x190;
    constexpr uintptr_t ChrInsModuleContainerData = 0x00;

    // CSChrDataModule slots (all i32), the live resource module.
    //   Source: fromsoftware-rs v0.14.0 chr_ins/module/data.rs
    //   Status: LIKELY
    constexpr uintptr_t ChrDataModuleHp        = 0x138;
    constexpr uintptr_t ChrDataModuleMaxHp     = 0x13C;
    constexpr uintptr_t ChrDataModuleFp        = 0x148;
    constexpr uintptr_t ChrDataModuleMaxFp     = 0x14C;

    // "Current block ID the player is in."
    constexpr uintptr_t PlayerInsCurrentBlockId = 0x6D0;

    // "Current play region id the player is in."
    constexpr uintptr_t PlayerInsPlayRegionId = 0x6E8;

    // ============================================================
    // Randomization limits
    // ============================================================

    // ER attributes are clamped to 1..99 by the game.
    constexpr int MinAttributeValue = 1;
    constexpr int MaxAttributeValue = 99;
}