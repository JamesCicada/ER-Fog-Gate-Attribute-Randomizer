#include "pch.h"

#include <Windows.h>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <cstdio>
#include <string>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <unordered_map>

#include "Version.h"
#include "Runtime.h"
#include "Scanner.h"
#include "PlayerOffsets.h"
#include "SeededRandom.h"


// ============================================================
// Logging
// ============================================================

// Allocates a console window attached to the game process and reroutes
// stdout/stderr to it, so log lines appear live in a terminal while the
// file log continues to persist for later review.
static bool g_consoleAttached = false;

void EnsureConsole()
{
    if (g_consoleAttached)
        return;

    if (AllocConsole())
    {
        FILE* stream = nullptr;

        freopen_s(&stream, "CONOUT$", "w", stdout);
        freopen_s(&stream, "CONOUT$", "w", stderr);
        freopen_s(&stream, "CONIN$", "r", stdin);

        SetConsoleTitleW(
            L"Fog Gate Randomizer - Live Log"
        );

        g_consoleAttached = true;
    }
}

void Log(const std::string& message)
{
    SYSTEMTIME time;
    GetLocalTime(&time);

    std::ostringstream line;

    line << "["
        << std::setfill('0')
        << std::setw(2) << time.wHour << ":"
        << std::setw(2) << time.wMinute << ":"
        << std::setw(2) << time.wSecond << "."
        << std::setw(3) << time.wMilliseconds
        << "] "
        << message;

    std::ofstream file(
        "FogGateRandomizer.log",
        std::ios::app
    );

    if (file.is_open())
    {
        file << line.str() << '\n';
    }

    if (g_consoleAttached)
    {
        std::cout << line.str() << std::endl;
    }
}


// ============================================================
// GameDataMan signature
// ============================================================

const char* GAMEDATAMAN_PATTERN =
"\x48\x8B\x05\x00\x00\x00\x00"
"\x48\x85\xC0"
"\x74\x05"
"\x48\x8B\x40\x58"
"\xC3\xC3";

const char* GAMEDATAMAN_MASK =
"xxx????xxxxxxxxxxx";


// ============================================================
// WorldChrMan signature
//
//   48 8B 05 ?? ?? ?? ??   mov rax, [rip+disp32]   ; load global
//   48 85 C0               test rax, rax
//   74 0F                  jz  +0xF
//   48 39 88 08 E5 01 00   cmp [rax+0x1E508], rcx  ; compare WorldChrMan.main_player
//
// The trailing 08 E5 01 00 is a cmp against [rax+0x1E508], which exactly
// matches WorldChrMan.main_player (v0.14.0 layout, ER 1.16.2). This longer
// signature is more discriminative than the short 48 39 88 form.
// ============================================================

const char* WORLDCHRMan_PATTERN =
"\x48\x8B\x05\x00\x00\x00\x00"
"\x48\x85\xC0"
"\x74\x0F"
"\x48\x39\x88\x08\xE5\x01\x00";

const char* WORLDCHRMan_MASK =
"xxx????xxxxxxxxxxxx";


// ============================================================
// Configuration (FogGateRandomizer.ini)
// ============================================================

struct ModConfig
{
    uint64_t globalSeed = 0x00C0FFEEULL;
    bool enableFogGateDetection = true;
    bool enableRuntimeApplication = false; // fail-closed by default
    bool enableBossDiagnostics = true;
    int  minAttribute = 1;
    int  maxAttribute = 99;
};

uint64_t ParseU64(const std::string& text)
{
    const char* begin = text.c_str();
    char* end = nullptr;

    if (begin[0] == '0' && (begin[1] == 'x' || begin[1] == 'X'))
    {
        return strtoull(begin, &end, 16);
    }

    return strtoull(begin, &end, 10);
}

std::string Trim(const std::string& text)
{
    size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return std::string();

    size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

bool LoadConfig(ModConfig& config)
{
    std::ifstream file("FogGateRandomizer.ini");

    if (!file.is_open())
    {
        Log("Config: FogGateRandomizer.ini not found, using defaults.");
        return false;
    }

    std::string line;

    while (std::getline(file, line))
    {
        std::string trimmed = Trim(line);

        if (trimmed.empty() ||
            trimmed[0] == '#' ||
            trimmed[0] == ';')
        {
            continue;
        }

        size_t equals = trimmed.find('=');

        if (equals == std::string::npos)
            continue;

        std::string key = Trim(trimmed.substr(0, equals));
        std::string value = Trim(trimmed.substr(equals + 1));

        if (key == "GlobalSeed")
            config.globalSeed = ParseU64(value);
        else if (key == "EnableFogGateDetection")
            config.enableFogGateDetection = (ParseU64(value) != 0);
        else if (key == "EnableRuntimeApplication")
            config.enableRuntimeApplication = (ParseU64(value) != 0);
        else if (key == "EnableBossDiagnostics")
            config.enableBossDiagnostics = (ParseU64(value) != 0);
        else if (key == "MinAttribute")
            config.minAttribute = static_cast<int>(ParseU64(value));
        else if (key == "MaxAttribute")
            config.maxAttribute = static_cast<int>(ParseU64(value));
    }

    if (config.minAttribute < 1)
        config.minAttribute = 1;

    if (config.maxAttribute > PlayerOffsets::MaxAttributeValue)
        config.maxAttribute = PlayerOffsets::MaxAttributeValue;

    if (config.maxAttribute < config.minAttribute)
        config.maxAttribute = config.minAttribute;

    {
        std::ostringstream line;
        line << "Config loaded | GlobalSeed: " << config.globalSeed
            << " | EnableFogGateDetection: "
            << (config.enableFogGateDetection ? "yes" : "no")
            << " | EnableRuntimeApplication: "
            << (config.enableRuntimeApplication ? "yes" : "no")
            << " | EnableBossDiagnostics: "
            << (config.enableBossDiagnostics ? "yes" : "no")
            << " | Attribute range: "
            << config.minAttribute << ".." << config.maxAttribute;
        Log(line.str());
    }

    return true;
}


// ============================================================
// Player state (GameDataMan StatBase; informational cross-check)
// ============================================================

struct PlayerState
{
    int vigor = 0;
    int mind = 0;
    int endurance = 0;
    int strength = 0;
    int dexterity = 0;
    int intelligence = 0;
    int faith = 0;
    int arcane = 0;
    int level = 0;

    bool operator==(const PlayerState& other) const
    {
        return
            vigor == other.vigor &&
            mind == other.mind &&
            endurance == other.endurance &&
            strength == other.strength &&
            dexterity == other.dexterity &&
            intelligence == other.intelligence &&
            faith == other.faith &&
            arcane == other.arcane &&
            level == other.level;
    }

    bool operator!=(const PlayerState& other) const
    {
        return !(*this == other);
    }
};

bool ReadPlayerState(
    uintptr_t gameDataMan,
    uintptr_t& statBase,
    PlayerState& state)
{
    if (!gameDataMan)
        return false;

    uintptr_t statBaseAddress =
        gameDataMan +
        PlayerOffsets::StatBasePtr;

    statBase =
        *reinterpret_cast<uintptr_t*>(
            statBaseAddress
            );

    if (!statBase)
        return false;

    state.vigor =
        *reinterpret_cast<int*>(statBase + PlayerOffsets::Vigor);
    state.mind =
        *reinterpret_cast<int*>(statBase + PlayerOffsets::Mind);
    state.endurance =
        *reinterpret_cast<int*>(statBase + PlayerOffsets::Endurance);
    state.strength =
        *reinterpret_cast<int*>(statBase + PlayerOffsets::Strength);
    state.dexterity =
        *reinterpret_cast<int*>(statBase + PlayerOffsets::Dexterity);
    state.intelligence =
        *reinterpret_cast<int*>(statBase + PlayerOffsets::Intelligence);
    state.faith =
        *reinterpret_cast<int*>(statBase + PlayerOffsets::Faith);
    state.arcane =
        *reinterpret_cast<int*>(statBase + PlayerOffsets::Arcane);
    state.level =
        *reinterpret_cast<int*>(statBase + PlayerOffsets::Level);

    return true;
}

void LogPlayerState(
    uintptr_t gameDataMan,
    uintptr_t statBase,
    const PlayerState& state)
{
    std::ostringstream line;

    line << "GameDataMan StatBase changed"
        << " | StatBase: 0x"
        << std::hex << std::uppercase
        << statBase
        << std::dec
        << " | Vigor: " << state.vigor
        << " | Mind: " << state.mind
        << " | Endurance: " << state.endurance
        << " | Strength: " << state.strength
        << " | Dexterity: " << state.dexterity
        << " | Intelligence: " << state.intelligence
        << " | Faith: " << state.faith
        << " | Arcane: " << state.arcane
        << " | Level: " << state.level;

    Log(line.str());
}


// ============================================================
// Authoritative active player
//
//   WorldChrMan + 0x1E508  -> main_player (PlayerIns)
//   PlayerIns  + 0x580     -> player_game_data (PlayerGameData)
//
// This is the authoritative "is the active local character loaded"
// signal. It REPLACES the heuristic of trusting GameDataMan + 0x08
// (which keeps the same StatBase address and overwrites it with
// transient data while characters load).
//
// The active-player gate is: main_player != 0 AND
//   chr_load_status == Active(2) AND is_active flag set AND
//   PlayerGameData.is_main_player == true.
//
// All offsets below are the v0.14.0 (ER 1.16.2) layouts --- see
// PlayerOffsets.h for the per-field status labels.
// ============================================================

struct ActivePlayerState
{
    bool valid = false;
    bool authoritative = false;

    uintptr_t worldChrMan = 0;
    uintptr_t mainPlayer = 0;
    uintptr_t chrSetEntry = 0;
    uintptr_t playerGameData = 0;

    unsigned int loadStatus = 0;
    bool isActive = false;
    bool isMainPlayer = false;
    bool isMyWorld = false;

    uint32_t characterEventId = 0;
    uint32_t characterId = 0;
    uint32_t blockId = 0;
    uint32_t playRegionId = 0;
    float blockPosition[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

    uintptr_t statBaseFromGameDataMan = 0;
    bool matchesStatBase = false;

    const char* reason = "";
};

ActivePlayerState ReadActivePlayer(
    uintptr_t worldChrManGlobal,
    uintptr_t gameDataMan)
{
    ActivePlayerState state;

    state.worldChrMan = Game::GetWorldChrManInstance(worldChrManGlobal);

    if (!state.worldChrMan)
    {
        state.reason = "no WorldChrMan";
        return state;
    }

    state.mainPlayer =
        *reinterpret_cast<uintptr_t*>(
            state.worldChrMan +
            PlayerOffsets::WorldChrManMainPlayer
            );

    if (!state.mainPlayer)
    {
        state.reason = "main_player null";
        return state;
    }

    state.chrSetEntry =
        *reinterpret_cast<uintptr_t*>(
            state.mainPlayer +
            PlayerOffsets::ChrInsChrSetEntry
            );

    if (!state.chrSetEntry)
    {
        state.reason = "no chr set entry";
        return state;
    }

    state.loadStatus =
        *reinterpret_cast<unsigned int*>(
            state.chrSetEntry +
            PlayerOffsets::ChrSetEntryLoadStatus
            );

    uint8_t flags1c8 =
        *reinterpret_cast<uint8_t*>(
            state.mainPlayer +
            PlayerOffsets::ChrInsFlags1C8
            );

    state.isActive =
        ((flags1c8 >> PlayerOffsets::ChrInsFlags1C8IsActiveBit) & 1) != 0;

    state.playerGameData =
        *reinterpret_cast<uintptr_t*>(
            state.mainPlayer +
            PlayerOffsets::PlayerInsPlayerGameData
            );

    if (!state.playerGameData)
    {
        state.reason = "no player game data";
        return state;
    }

    state.isMainPlayer =
        *reinterpret_cast<bool*>(
            state.playerGameData +
            PlayerOffsets::IsMainPlayer
            );

    state.isMyWorld =
        *reinterpret_cast<bool*>(
            state.playerGameData +
            PlayerOffsets::IsMyWorld
            );

    state.characterEventId =
        *reinterpret_cast<uint32_t*>(
            state.playerGameData +
            PlayerOffsets::CharacterEventId
            );

    state.characterId =
        *reinterpret_cast<uint32_t*>(
            state.playerGameData +
            PlayerOffsets::CharacterId
            );

    state.blockId =
        *reinterpret_cast<uint32_t*>(
            state.mainPlayer +
            PlayerOffsets::PlayerInsCurrentBlockId
            );

    state.playRegionId =
        *reinterpret_cast<uint32_t*>(
            state.mainPlayer +
            PlayerOffsets::PlayerInsPlayRegionId
            );

    const float* blockPos =
        reinterpret_cast<const float*>(
            state.mainPlayer +
            PlayerOffsets::PlayerInsBlockPosition
            );

    state.blockPosition[0] = blockPos[0];
    state.blockPosition[1] = blockPos[1];
    state.blockPosition[2] = blockPos[2];
    state.blockPosition[3] = blockPos[3];

    if (gameDataMan)
    {
        uintptr_t statBaseSlot =
            gameDataMan +
            PlayerOffsets::StatBasePtr;

        state.statBaseFromGameDataMan =
            *reinterpret_cast<uintptr_t*>(
                statBaseSlot
                );

        state.matchesStatBase =
            state.statBaseFromGameDataMan == state.playerGameData;
    }

    // ------------------------------------------------------------------
    // Authoritative gate: main character loaded and script-active.
    // ------------------------------------------------------------------

    state.valid = true;

    if (state.loadStatus != PlayerOffsets::ChrLoadStatusActive)
    {
        state.reason = "chr load status not Active";
    }
    else if (!state.isActive)
    {
        state.reason = "is_active flag not set";
    }
    else if (!state.isMainPlayer)
    {
        state.reason = "PlayerGameData.is_main_player false";
    }
    else
    {
        state.authoritative = true;
        state.reason = "active";
    }

    return state;
}


// ============================================================
// Fog gate detection
//
// Two runtime anchors are used:
//
//   A) boss_fight_active rising edge (GameDataMan + 0xC0).
//      "Whether a boss fight is currently active". Entering a boss
//      fog gate raises this flag, and GameDataMan + 0xCC identifies
//      the activated boss (stable per-gate identity component).
//      Status: LIKELY (v0.14.0 layout; runtime confirm pending).
//
//   B) Authoritative-player load transitions (chr_load_status leaving
//      and returning to Active). Log-only / diagnostic; NOT emitted as
//      a gate, because fast travel / respawn also load without being a
//      fog gate.
//
// The preferred per-gate identity would be the fog wall asset entity
// id (event 9005800 argument X4_4). That value is not yet runtime-
// readable without an event hook --- reading it is the documented
// upgrade path (see StableGateIdentity::fogWallEntityId).
// ============================================================

struct BossGateEvent
{
    bool valid = false;
    StableGateIdentity gate;
    uint32_t destinationBlock = 0;
    uint32_t destinationRegion = 0;
};

struct FogGateDetector
{
    bool wasAuthoritative = false;
    bool wasBossFightActive = false;

    uint32_t previousBlock = 0;
    uint32_t previousRegion = 0;
    uint32_t previousCharacterEventId = 0;

    // Adjusts the emitted gate identity by the position where the
    // traversal was detected, quantized so tiny float differences
    // cannot create a different gate.
    void AddQuantizedPosition(
        StableGateIdentity& gate,
        const float position[4]) const
    {
        const float unit = 256.0f;

        gate.quantizedX =
            static_cast<int32_t>(std::floor(position[0] / unit));
        gate.quantizedY =
            static_cast<int32_t>(std::floor(position[1] / unit));
        gate.quantizedZ =
            static_cast<int32_t>(std::floor(position[2] / unit));
    }

    BossGateEvent Update(
        const ActivePlayerState& player,
        bool bossFightActive,
        uint32_t bossNpcParamId,
        bool enableDetection)
    {
        BossGateEvent event;

        if (!enableDetection)
        {
            wasAuthoritative = player.authoritative;
            wasBossFightActive = bossFightActive;
            previousBlock = player.blockId;
            previousRegion = player.playRegionId;
            previousCharacterEventId = player.characterEventId;
            return event;
        }

        // Tracking state for diagnostics / character switches.
        if (wasAuthoritative && !player.authoritative)
        {
            Log("Player left active state (loading/unloading).");
        }
        else if (!wasAuthoritative && player.authoritative)
        {
            std::ostringstream line;

            line << "Player entered active state"
                << " | character_event_id: "
                << player.characterEventId
                << " | block: "
                << player.blockId
                << " | play_region: "
                << player.playRegionId;

            if (previousCharacterEventId != 0 &&
                player.characterEventId != previousCharacterEventId)
            {
                line << " | CHARACTER SWITCHED from "
                    << previousCharacterEventId;
            }

            Log(line.str());
        }

        // Anchor A: boss fog gate traversal.
        if (bossFightActive && !wasBossFightActive && player.authoritative)
        {
            event.valid = true;

            event.gate.fogWallEntityId = 0; // requires event hook (NOT VERIFIED)
            event.gate.blockId = player.blockId;
            event.gate.playRegionId = player.playRegionId;
            event.gate.bossNpcParamId = bossNpcParamId;

            AddQuantizedPosition(event.gate, player.blockPosition);

            event.destinationBlock = player.blockId;
            event.destinationRegion = player.playRegionId;

            std::ostringstream line;

            line << "Boss fog gate traversal detected"
                << " | character_event_id: "
                << player.characterEventId
                << " | block: "
                << player.blockId
                << " | play_region: "
                << player.playRegionId
                << " | boss_npc_param_id: "
                << bossNpcParamId;

            Log(line.str());
        }

        wasAuthoritative = player.authoritative;
        wasBossFightActive = bossFightActive;
        previousBlock = player.blockId;
        previousRegion = player.playRegionId;
        previousCharacterEventId = player.characterEventId;

        return event;
    }
};


// ============================================================
// Application of a gate result (runtime-only, fail-closed)
// ============================================================

struct GateApplicationResult
{
    int applied = 0;
    int rejected = 0;
    bool accepted = false;
};

// Writes the deterministic attribute set to the PlayerGameData
// runtime-computed ("effective") attribute fields. These fields are
// recomputed by the game from equipment/buffs, so this is a runtime
// overlay, NOT a save-file modification.
//
// Each write is range-validated against the ER attribute cap and only
// performed through the verified PlayerIns -> PlayerGameData chain.
GateApplicationResult ApplyGateResult(
    const ActivePlayerState& player,
    const GateAttributeResult& gateResult,
    bool enableApplication)
{
    GateApplicationResult outcome;
    outcome.accepted = false;

    if (!enableApplication)
    {
        return outcome;
    }

    if (!player.authoritative || !player.playerGameData)
    {
        outcome.rejected = PlayerOffsets::AttributeCount;
        return outcome;
    }

    static constexpr uintptr_t EffectiveOffsetsInOrder[PlayerOffsets::AttributeCount] = {
        PlayerOffsets::EffectiveVigor,
        PlayerOffsets::EffectiveMind,
        PlayerOffsets::EffectiveEndurance,
        PlayerOffsets::EffectiveStrength,
        PlayerOffsets::EffectiveDexterity,
        PlayerOffsets::EffectiveIntelligence,
        PlayerOffsets::EffectiveFaith,
        PlayerOffsets::EffectiveArcane,
    };

    for (int i = 0; i < PlayerOffsets::AttributeCount; ++i)
    {
        int value = gateResult.attributes[i];

        if (value < PlayerOffsets::MinAttributeValue ||
            value > PlayerOffsets::MaxAttributeValue)
        {
            ++outcome.rejected;
            continue;
        }

        *reinterpret_cast<uint32_t*>(
            player.playerGameData + EffectiveOffsetsInOrder[i]
            ) = static_cast<uint32_t>(value);

        ++outcome.applied;
    }

    outcome.accepted = (outcome.rejected == 0);
    return outcome;
}


// ============================================================
// Main runtime
// ============================================================

DWORD WINAPI MainThread(LPVOID)
{
    EnsureConsole();

    // --------------------------------------------------------
    // Timestamp this test run
    // --------------------------------------------------------

    SYSTEMTIME time;
    GetLocalTime(&time);

    {
        std::ostringstream timestamp;

        timestamp << "===== Log started at "
            << std::setfill('0')
            << std::setw(2) << time.wHour << ":"
            << std::setw(2) << time.wMinute << ":"
            << std::setw(2) << time.wSecond
            << " =====";

        Log(timestamp.str());
    }

    // --------------------------------------------------------
    // Initialization
    // --------------------------------------------------------

    Log("========================================");
    Log("Fog Gate Randomizer initialized");

    Log("Target App: " +
        std::string(Game::ExpectedAppVersion));

    Log("Target Calibration: " +
        std::string(Game::ExpectedCalibrationVersion));

    Log("========================================");

    ModConfig config;
    LoadConfig(config);

    if (config.enableRuntimeApplication)
    {
        Log("WARNING: EnableRuntimeApplication is ON. "
            "This writes to runtime-computed stat fields and is "
            "intended for testing only.");
    }


    // --------------------------------------------------------
    // Get main game module
    // --------------------------------------------------------

    auto module = Game::GetMainModule();

    if (module.baseAddress == 0)
    {
        Log("ERROR: Could not find main game module.");
        return 0;
    }

    {
        std::ostringstream address;

        address << "Game base address: 0x"
            << std::hex
            << std::uppercase
            << module.baseAddress;

        Log(address.str());
    }

    {
        std::ostringstream size;

        size << "Game image size: 0x"
            << std::hex
            << std::uppercase
            << module.imageSize;

        Log(size.str());
    }


    // --------------------------------------------------------
    // Find GameDataMan signature
    // --------------------------------------------------------

    uintptr_t gameDataManInstruction =
        Game::FindPattern(
            GAMEDATAMAN_PATTERN,
            GAMEDATAMAN_MASK
        );

    if (!gameDataManInstruction)
    {
        Log("ERROR: GameDataMan signature not found.");
        return 0;
    }

    {
        std::ostringstream address;

        address << "GameDataMan instruction: 0x"
            << std::hex
            << std::uppercase
            << gameDataManInstruction;

        Log(address.str());
    }

    uintptr_t gameDataManGlobal =
        Game::ResolveRIPRelative(
            gameDataManInstruction
        );

    if (!gameDataManGlobal)
    {
        Log("ERROR: Failed to resolve GameDataMan global pointer.");
        return 0;
    }

    {
        std::ostringstream address;

        address << "GameDataMan global: 0x"
            << std::hex
            << std::uppercase
            << gameDataManGlobal;

        Log(address.str());
    }

    Log("Waiting for GameDataMan to initialize...");

    uintptr_t gameDataMan = 0;

    constexpr DWORD PollMilliseconds = 100;

    while (true)
    {
        gameDataMan =
            Game::GetGameDataManInstance(
                gameDataManGlobal
            );

        if (gameDataMan)
            break;

        Sleep(PollMilliseconds);
    }

    {
        std::ostringstream address;

        address << "GameDataMan instance: 0x"
            << std::hex
            << std::uppercase
            << gameDataMan;

        Log(address.str());
    }


    // --------------------------------------------------------
    // Find WorldChrMan signature
    // --------------------------------------------------------

    Log("Scanning for WorldChrMan signature...");

    uintptr_t worldChrManMatches[8];
    size_t worldChrManMatchCount =
        Game::FindPatternAll(
            WORLDCHRMan_PATTERN,
            WORLDCHRMan_MASK,
            worldChrManMatches,
            8
        );

    {
        std::ostringstream count;

        count << "WorldChrMan signature matches: "
            << std::dec
            << worldChrManMatchCount;

        Log(count.str());
    }

    for (size_t i = 0;
        i < worldChrManMatchCount && i < 8;
        ++i)
    {
        std::ostringstream address;

        address << "  Match " << std::dec << i
            << ": 0x"
            << std::hex
            << std::uppercase
            << worldChrManMatches[i];

        Log(address.str());
    }

    if (worldChrManMatchCount == 0)
    {
        Log("ERROR: WorldChrMan signature not found.");
        return 0;
    }

    if (worldChrManMatchCount != 1)
    {
        Log("WARNING: WorldChrMan signature is ambiguous (not unique). "
            "Proceeding with the first match; confirm from the logs.");

        for (size_t i = 0;
            i < worldChrManMatchCount && i < 8;
            ++i)
        {
            std::ostringstream surrounding;

            surrounding << "  Match " << std::dec << i
                << " bytes:";

            for (int b = 0; b < 16; ++b)
            {
                surrounding << " "
                    << std::hex
                    << std::uppercase
                    << std::setw(2)
                    << std::setfill('0')
                    << static_cast<unsigned>(
                        *reinterpret_cast<unsigned char*>(
                            worldChrManMatches[i] + b
                            ));
            }

            Log(surrounding.str());
        }
    }

    uintptr_t worldChrManInstruction = worldChrManMatches[0];

    {
        std::ostringstream address;

        address << "WorldChrMan instruction: 0x"
            << std::hex
            << std::uppercase
            << worldChrManInstruction;

        Log(address.str());
    }

    uintptr_t worldChrManGlobal =
        Game::ResolveRIPRelative(
            worldChrManInstruction
        );

    if (!worldChrManGlobal)
    {
        Log("ERROR: Failed to resolve WorldChrMan global pointer.");
        return 0;
    }

    {
        std::ostringstream address;

        address << "WorldChrMan global: 0x"
            << std::hex
            << std::uppercase
            << worldChrManGlobal;

        Log(address.str());
    }

    Log("Waiting for WorldChrMan to initialize...");

    uintptr_t worldChrMan = 0;

    while (true)
    {
        worldChrMan =
            Game::GetWorldChrManInstance(
                worldChrManGlobal
            );

        if (worldChrMan)
            break;

        Sleep(PollMilliseconds);
    }

    {
        std::ostringstream address;

        address << "WorldChrMan instance: 0x"
            << std::hex
            << std::uppercase
            << worldChrMan;

        Log(address.str());
    }


    // --------------------------------------------------------
    // Continuous monitor
    //
    // Change/event-based logging; no per-frame spam in normal mode.
    // --------------------------------------------------------

    Log("Starting monitor...");

    FogGateDetector detector;

    PlayerState previousState;
    uintptr_t previousStatBase = 0;
    bool havePreviousState = false;
    bool previousAuthoritative = false;

    // Diagnostics: track raw boss-fight-state transitions so we can see
    // whether the boss_fight_active / boss_npc_param_id anchors (LIKELY
    // offsets) actually toggle at a boss fog wall. This is the fastest way
    // to validate or correct those offsets.
    bool haveBossDiag = false;
    bool previousBossFightActive = false;
    uint32_t previousBossNpcParamId = 0;

    // Cache of applied gate results (demonstrates the determinism path;
    // the deterministic re-computation is the source of truth).
    std::unordered_map<uint64_t, GateAttributeResult> gateResultCache;

    while (true)
    {
        // ----------------------------------------------------
        // Re-read GameDataMan every iteration.
        // ----------------------------------------------------

        uintptr_t currentGameDataMan =
            Game::GetGameDataManInstance(
                gameDataManGlobal
            );

        if (!currentGameDataMan)
        {
            if (havePreviousState)
            {
                Log("GameDataMan became unavailable.");
                havePreviousState = false;
                previousStatBase = 0;
            }
        }
        else
        {
            // Informational: GameDataMan + 0x08 StatBase. Note this
            // pointer is NOT a reliable active-character detector (the
            // same address is reused and overwritten during loading).
            PlayerState currentState;
            uintptr_t currentStatBase = 0;

            if (ReadPlayerState(
                currentGameDataMan,
                currentStatBase,
                currentState))
            {
                bool statBaseChanged = currentStatBase != previousStatBase;
                bool valuesChanged = !havePreviousState ||
                    currentState != previousState;

                if (statBaseChanged || valuesChanged)
                {
                    LogPlayerState(
                        currentGameDataMan,
                        currentStatBase,
                        currentState);
                }

                previousState = currentState;
                previousStatBase = currentStatBase;
                havePreviousState = true;
            }
            else if (havePreviousState)
            {
                Log("Player data became unavailable.");
                havePreviousState = false;
                previousStatBase = 0;
            }
        }


        // ----------------------------------------------------
        // Authoritative active player (WorldChrMan chain).
        // ----------------------------------------------------

        ActivePlayerState player =
            ReadActivePlayer(
                worldChrManGlobal,
                currentGameDataMan
            );

        if (player.authoritative != previousAuthoritative)
        {
            std::ostringstream line;

            line << "Active player "
                << (player.authoritative ? "ACQUIRED" : "LOST (authoritative)")
                << " | reason: "
                << player.reason;

            if (player.valid)
            {
                line << " | main_player: 0x"
                    << std::hex << std::uppercase
                    << player.mainPlayer
                    << " | player_game_data: 0x"
                    << player.playerGameData
                    << std::dec
                    << " | load_status: "
                    << player.loadStatus
                    << " | is_active: "
                    << (player.isActive ? "true" : "false")
                    << " | is_main_player: "
                    << (player.isMainPlayer ? "true" : "false")
                    << " | is_my_world: "
                    << (player.isMyWorld ? "true" : "false")
                    << " | character_event_id: "
                    << player.characterEventId
                    << " | block: "
                    << player.blockId
                    << " | play_region: "
                    << player.playRegionId
                    << " | matches GameDataMan StatBase: "
                    << (player.matchesStatBase ? "YES" : "NO")
                    << " | GameDataMan StatBase: 0x"
                    << std::hex << std::uppercase
                    << player.statBaseFromGameDataMan
                    << std::dec;
            }

            Log(line.str());

            previousAuthoritative = player.authoritative;
        }


        // ----------------------------------------------------
        // Fog gate detection.
        // ----------------------------------------------------

        uint32_t bossNpcParamId = 0;
        bool bossFightActive = false;

        if (currentGameDataMan)
        {
            bossFightActive =
                *reinterpret_cast<bool*>(
                    currentGameDataMan +
                    PlayerOffsets::GameDataManBossFightActive
                    );

            bossNpcParamId =
                *reinterpret_cast<uint32_t*>(
                    currentGameDataMan +
                    PlayerOffsets::GameDataManBossNpcParamId
                    );
        }

        BossGateEvent event =
            detector.Update(
                player,
                bossFightActive,
                bossNpcParamId,
                config.enableFogGateDetection
            );

        // Diagnostic: log any change to the raw boss-fight fields (from the
        // LIKELY offsets 0xC0 / 0xCC). Reveals whether these anchors toggle
        // at a boss fog wall, so we can validate/correct the offsets.
        if (currentGameDataMan && config.enableBossDiagnostics)
        {
            if (!haveBossDiag ||
                bossFightActive != previousBossFightActive ||
                bossNpcParamId != previousBossNpcParamId)
            {
                std::ostringstream diag;

                diag << "BOSS_DIAG boss_fight_active: "
                    << (bossFightActive ? "true" : "false")
                    << " | boss_npc_param_id: "
                    << bossNpcParamId
                    << " (offsets 0xC0/0xCC, LIKELY)";

                Log(diag.str());
            }

            haveBossDiag = true;
            previousBossFightActive = bossFightActive;
            previousBossNpcParamId = bossNpcParamId;
        }

        if (event.valid)
        {
            // --------------------------------------------------
            // Deterministic gate result.
            // --------------------------------------------------

            uint64_t gateKey = event.gate.Key();

            GateAttributeResult computed =
                ComputeGateResult(
                    config.globalSeed,
                    event.gate,
                    config.minAttribute,
                    config.maxAttribute
                );

            bool cached = false;

            auto cachedIt = gateResultCache.find(gateKey);

            if (cachedIt != gateResultCache.end())
            {
                cached = (cachedIt->second == computed);
            }
            else
            {
                gateResultCache.emplace(gateKey, computed);
            }

            std::ostringstream line;

            line << "Gate randomization"
                << " | gate_key: 0x"
                << std::hex << std::uppercase
                << gateKey
                << std::dec
                << " | gate_seed: 0x"
                << std::hex << std::uppercase
                << computed.gateSeed
                << std::dec
                << " | block: "
                << event.gate.blockId
                << " | play_region: "
                << event.gate.playRegionId
                << " | boss_npc_param_id: "
                << event.gate.bossNpcParamId
                << " | pos: ("
                << event.gate.quantizedX << ", "
                << event.gate.quantizedY << ", "
                << event.gate.quantizedZ << ")"
                << " | attributes: "
                << computed.attributes[0] << " "
                << computed.attributes[1] << " "
                << computed.attributes[2] << " "
                << computed.attributes[3] << " "
                << computed.attributes[4] << " "
                << computed.attributes[5] << " "
                << computed.attributes[6] << " "
                << computed.attributes[7]
                << " | from cache (identical result): "
                << (cached ? "yes" : "no");

            Log(line.str());

            GateApplicationResult applied =
                ApplyGateResult(
                    player,
                    computed,
                    config.enableRuntimeApplication
                );

            if (applied.accepted)
            {
                std::ostringstream appliedLine;

                appliedLine << "Gate result applied successfully"
                    << " | fields written: "
                    << applied.applied;

                Log(appliedLine.str());
            }
            else if (config.enableRuntimeApplication)
            {
                std::ostringstream rejectedLine;

                rejectedLine << "Gate result NOT fully applied"
                    << " (fail-closed)"
                    << " | written: "
                    << applied.applied
                    << " | rejected: "
                    << applied.rejected;

                Log(rejectedLine.str());
            }
        }


        Sleep(PollMilliseconds);
    }


    // Never reached
    return 0;
}


// ============================================================
// DLL Entry Point
// ============================================================

BOOL APIENTRY DllMain(
    HMODULE hModule,
    DWORD reason,
    LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);

        HANDLE thread =
            CreateThread(
                nullptr,
                0,
                MainThread,
                nullptr,
                0,
                nullptr
            );

        if (thread)
        {
            CloseHandle(thread);
        }
    }

    return TRUE;
}