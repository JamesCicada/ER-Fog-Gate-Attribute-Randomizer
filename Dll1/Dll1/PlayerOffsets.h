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