#include "pch.h"

#include <Windows.h>
#include <bcrypt.h>
#include <cstdlib>
#include <cstring>
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

// Returns the directory containing this DLL, so config/log files are always
// resolved relative to the DLL rather than the (unpredictable) game CWD.
std::string GetModuleDirectory()
{
    char path[MAX_PATH] = { 0 };
    HMODULE mod = nullptr;

    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&GetModuleDirectory),
        &mod);

    if (mod)
    {
        GetModuleFileNameA(mod, path, MAX_PATH);
    }
    else
    {
        GetModuleFileNameA(nullptr, path, MAX_PATH);
    }

    std::string full(path);

    size_t slash = full.find_last_of("\\/");

    if (slash != std::string::npos)
    {
        full = full.substr(0, slash + 1);
    }

    return full;
}

// Returns "<dll_dir>BossStatsRandomizer\" (creating the folder if needed).
// All config/log files are kept in this subfolder so they live next to the
// DLL (e.g. <gameroot>/mods/BossStatsRandomizer/) rather than in the game CWD.
std::string StatsRandomizerDir()
{
    std::string dir = GetModuleDirectory() + "BossStatsRandomizer\\";

    CreateDirectoryA(dir.c_str(), nullptr);

    return dir;
}

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
            L"Boss Stats Randomizer - Live Log"
        );

        g_consoleAttached = true;
    }
}

// Log level: 0 = debug (log everything), 1 = errors only (default).
// Initialized to 1 so nothing logs until config is loaded; LoadConfig
// then applies the LogLevel from the ini.
int g_logLevel = 1;

void Log(const std::string& message, int level = 0)
{
    if (level > g_logLevel)
    {
        return;
    }

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
        << (level >= 1 ? "ERROR: " : "")
        << message;

    std::ofstream file(
        StatsRandomizerDir() + "BossStatsRandomizer.log",
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

void LogError(const std::string& message)
{
    Log(message, 1);
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
// EstusAllocationUpdate signature
//
// Address-equivalent of TGA's "Add charge to flask" helper. The game reads
// PlayerGameData+0x101 (byHp) / +0x102 (byMp) for the allocated flask counts,
// but a bare byte write does NOT touch the live flask counters during a fight.
// This internal routine does (the CT invokes it with executeCodeEx to make a
// new charge apply immediately). Signature: (int flaskType 0=HP/1=FP, int count).
//
// Pattern ripped from TGA Cheat Table "Scripts/Build Creation/Add charge to
// flask.cea" (authoritative for ER 1.16.2). Only the byte layout is trusted;
// the address itself is resolved at runtime on this game's module.
// ============================================================

const char* ESTUS_ALLOC_UPDATE_PATTERN =
"\x00\x8B\x00\x00\x00\x00\x00\x00\x8B\xC1\x00\x8B\x00\x00\x00"
"\x85\xC9\x74\x00\x00\x85\xC0\x74\x00\x00\x83\xF8\x01\x75\x00\x0F\xB6";

// 32 tokens (not 33): the CT pattern is
//   ?? 8b ?? ?? ?? ?? ?? ?? 8b c1 ?? 8b ?? ?? ?? 85 c9 74 ?? ?? 85 c0 74
//   ?? ?? 83 f8 01 75 ?? 0f b6
// i.e. known bytes at indexes 1,8,9,11,15,16,17,20,21,22,25,26,27,28,30,31.
// The old mask/pattern were one byte too long, so the scan never matched and
// the engine call below was never reached (charges stayed 10 red / 8 blue).
const char* ESTUS_ALLOC_UPDATE_MASK =
"?x??????xx?x???xxx??xxx??xxxx?xx";

// Resolved once at startup; stays 0 (bytes-only fallback) if not found.
uintptr_t g_estusAllocationUpdate = 0;
bool      g_estusUpdateReady = false;

// ============================================================
// Flask level (item-id + bonfire level) signatures
//
// The flask "+X" upgrade is not stored in PlayerGameData: it lives in the
// flask ITEM ids (Crimson 1000, Cerulean 1001, Physick 1050/1051, each
// + level*2) and a "total bonfire level" byte. TGA's "Set flask level" script
// (authoritative for ER 1.16.2) upgrades them via three pieces:
//
//   ActivateBonfire(AOB)        : sets the bonfire-level byte.
//   ReplaceTool(AOB found-0x19) : swaps an inventory item id to a new one.
//   GetTotalBonfireLevel        : byte at ActivateBonfire+0x44 + rel32 + 5
//                                 (the script's readInteger(+0x44+2) is the
//                                 disp32 of that RIP-relative instruction).
//
// We drop the script's SaveRequest (GameMan+0xB72=1) so the randomization
// stays memory-only and can be reversed at the end of the fight.
// ============================================================

// 88 4C ?? ?? 53 ?? 83 EC ?? C6 44 ?? ?? 63 C6 44 ?? ?? 01 80 F9 01
const char* BONFIRE_LEVEL_SET_PATTERN =
"\x88\x4C\x00\x00\x53\x00\x83\xEC\x00\xC6\x44\x00\x00\x63\xC6\x44\x00\x00\x01\x80\xF9\x01";

const char* BONFIRE_LEVEL_SET_MASK =
"xx??x?xx?xx??xxx??xxxx";

// ?? 0F B6 F1 ?? 8B D8 ?? 8B F9 81 E2 FF FF FF 0F 0F BA EA ?? 89 54 ?? ?? ?? 81 C1
// Pattern starts inside the function body; the entry point is 0x19 bytes before.
const char* REPLACE_TOOL_PATTERN =
"\x00\x0F\xB6\xF1\x00\x8B\xD8\x00\x8B\xF9\x81\xE2\xFF\xFF\xFF\x0F\x0F\xBA\xEA\x00\x89\x54\x00\x00\x00\x81\xC1";

const char* REPLACE_TOOL_MASK =
"?xxx?xx?xxxxxxxxxxx?xx???xx";

uintptr_t g_activateBonfire = 0;
uintptr_t g_replaceTool = 0;
uintptr_t g_totalBonfireLevelAddr = 0;
bool      g_flaskLevelReady = false;


// ============================================================
// Configuration (BossStatsRandomizer.ini)
// ============================================================

struct ModConfig
{
    uint64_t globalSeed = 0x00C0FFEEULL;
    bool enableFogGateDetection = true;
    bool enableRuntimeApplication = false; // fail-closed by default
    bool enableBossDiagnostics = true;
    bool cacheInitialStats = true;
    bool randomizeWondrousPhysick = false; // off by default (user directive)
    bool wondrousPhysickIncludeDlc = false; // include Shadow of the Erdtree tears
    int  logLevel = 1; // 0 = debug (everything), 1 = errors only
    int  minAttribute = 1;
    int  maxAttribute = 99;

    // Per-attribute cap, applied on top of maxAttribute. Each entry is
    // independent: the effective cap for an attribute is
    // min(maxAttribute, perAttribute). 99 = no per-attribute limit.
    int maxAttributeByIndex[PlayerOffsets::AttributeCount] = {
        99, 99, 99, 99, 99, 99, 99, 99
    };

    bool  randomizeFlaskCharges = true;
    bool  randomizeFlaskLevel = true;
    int   minFlaskCharges = 2; // combined HP+FP total range during fights
    int   maxFlaskCharges = 14; // game cap is 14 combined
    int   minFlaskLevel = 1;
    int   maxFlaskLevel = 12;

    // When a gate result is applied (and restored), also top the player's
    // HP / FP back up to their current maxima.
    bool  fillVigorOnRandomize = true;
    bool  fillFpOnRandomize = true;

    // 1 = spawn the "Live Log" console window; 0 = headless (default).
    bool  showConsole = false;
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

// Config key suffix -> attribute index (PlayerOffsets::AttributeOrder
// compatible). Returns -1 when the name is not an attribute.
int AttributeIndexFromName(const std::string& name)
{
    static const char* kNames[PlayerOffsets::AttributeCount] = {
        "Vigor", "Mind", "Endurance", "Strength",
        "Dexterity", "Intelligence", "Faith", "Arcane"
    };

    for (int i = 0; i < PlayerOffsets::AttributeCount; ++i)
    {
        if (name == kNames[i])
            return i;
    }

    return -1;
}

// Effective per-attribute cap: min(globalMax, perAttribute), floored at
// the global minimum so the config can never produce an under-range stat.
void ResolveAttributeCaps(const ModConfig& config, int* outMaxByIndex)
{
    for (int i = 0; i < PlayerOffsets::AttributeCount; ++i)
    {
        int value = config.maxAttributeByIndex[i];

        if (config.maxAttribute < value)
        {
            value = config.maxAttribute;
        }

        if (value < config.minAttribute)
        {
            value = config.minAttribute;
        }

        outMaxByIndex[i] = value;
    }
}

// Saved copy of the flask state (charges + heal potency + bonfire level)
// captured when a gate result is first applied, restored when the boss
// fight ends.
struct SavedFlaskState
{
    uint8_t hpCharges = 0;
    uint8_t fpCharges = 0;
    float   hpRate = 0.0f;
    uint8_t hpAdditional = 0;
    float   fpRate = 0.0f;
    uint8_t fpAdditional = 0;
    int     bonfireLevel = -1; // 1-based total bonfire level, -1 = unknown
};

uint32_t FloatToBits(float value)
{
    uint32_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float BitsToFloat(uint32_t bits)
{
    float value = 0.0f;
    memcpy(&value, &bits, sizeof(bits));
    return value;
}

uint64_t GenerateRandomSeed()
{
    uint64_t seed = 0;

    BCryptGenRandom(
        nullptr,
        reinterpret_cast<PUCHAR>(&seed),
        static_cast<ULONG>(sizeof(seed)),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);

    if (seed == 0)
    {
        seed = static_cast<uint64_t>(GetTickCount64());
    }

    return seed;
}

void WriteConfigDefaults(uint64_t seed)
{
    std::ofstream file(
        StatsRandomizerDir() + "BossStatsRandomizer.ini",
        std::ios::trunc);

    if (!file.is_open())
    {
        Log("Config: could not write BossStatsRandomizer.ini.");
        return;
    }

    file
        << "; ============================================================================\n"
        << "; Boss Stats Randomizer\n"
        << ";\n"
        << "; Randomizes your character's stats every time you enter a boss fog gate.\n"
        << "; The result is deterministic: same GlobalSeed + same boss = same stats.\n"
        << "; Everything is restored (or re-randomized) automatically, and every setting\n"
        << "; below is optional. All lines starting with ';' are comments.\n"
        << "; ============================================================================\n"
        << "\n"
        << "[General]\n"
        << "\n"
        << "; Master seed for all randomization. The mod picks a random one on first\n"
        << "; launch; change it to re-seed every boss's stats.\n"
        << "GlobalSeed=" << seed << "\n"
        << "\n"
        << "; 0 = log errors only (quiet), 1 = debug (everything).\n"
        << "LogLevel=1\n"
        << "\n"
        << "; 1 = open a 'Live Log' console window next to the game, 0 = headless.\n"
        << "ShowConsole=0\n"
        << "\n"
        << "; 1 = verbose boss-fight state diagnostics in the log. Only useful when\n"
        << "; debugging detection or offsets.\n"
        << "EnableBossDiagnostics=1\n"
        << "\n"
        << "\n"
        << "[Gate Detection]\n"
        << "\n"
        << "; Detect boss-fight activation (fog gate entry) to fire randomization.\n"
        << "EnableFogGateDetection=1\n"
        << "\n"
        << "\n"
        << "[Stat Randomization]\n"
        << "\n"
        << "; Write the randomized stats into the game during boss fights. Must be on\n"
        << "; for any randomization to happen.\n"
        << "EnableRuntimeApplication=1\n"
        << "\n"
        << "; Cache your original stats before randomizing and restore them when the\n"
        << "; fight ends. 0 = randomized stats persist for the whole session.\n"
        << "; The cache also enables crash recovery (a quit mid-fight is restored on\n"
        << "; the next launch).\n"
        << "CacheInitialStats=1\n"
        << "\n"
        << "; Randomized stat range. Every stat is rolled between MinAttribute and\n"
        << "; MaxAttribute (1..99).\n"
        << "MinAttribute=1\n"
        << "MaxAttribute=99\n"
        << "\n"
        << "; Per-attribute upper caps, independent of each other. The effective cap\n"
        << "; for each stat is min(MaxAttribute, its own cap). 99 = no extra limit.\n"
        << "MaxVigor=99\n"
        << "MaxMind=99\n"
        << "MaxEndurance=99\n"
        << "MaxStrength=99\n"
        << "MaxDexterity=99\n"
        << "MaxIntelligence=99\n"
        << "MaxFaith=99\n"
        << "MaxArcane=99\n"
        << "\n"
        << "\n"
        << "[Flask Randomization]\n"
        << "\n"
        << "; Randomize your flask loadout during boss fights. Charges and recovery\n"
        << "; are deterministic per boss. Both are restored when the fight ends.\n"
        << "RandomizeFlaskCharges=1\n"
        << "RandomizeFlaskLevel=1\n"
        << "\n"
        << "; Charge range as a COMBINED total (Crimson + Cerulean charges together).\n"
        << "; The game's cap is 14 total. 0 is allowed (no flasks at all - hard mode).\n"
        << "MinFlaskCharges=2\n"
        << "MaxFlaskCharges=14\n"
        << "\n"
        << "; Flask power range (1..12, the 'sacred tear' level equivalent). Higher =\n"
        << "; more HP/FP healed per flask.\n"
        << "MinFlaskLevel=1\n"
        << "MaxFlaskLevel=12\n"
        << "\n"
        << "\n"
        << "[Wondrous Physick]\n"
        << "\n"
        << "; Randomize the tears mixed into your Wondrous Physick during boss fights.\n"
        << "; OFF by default. When on, the two flasks are filled with a random pick\n"
        << "; from the full pool of crystal tears, seeded per boss.\n"
        << "RandomizeWondrousPhysick=0\n"
        << "\n"
        << "; Include the 8 Shadow of the Erdtree crystal tears in the random pool.\n"
        << "; Requires the DLC. 0 = base-game tears only.\n"
        << "WondrousPhysickIncludeDlc=0\n"
        << "\n"
        << "\n"
        << "[Healing]\n"
        << "\n"
        << "; Top your HP / FP back up to full right after stats are randomized (and\n"
        << "; for a short settling window so the game doesn't instantly overwrite it)\n"
        << "; and again when they are restored after the fight.\n"
        << "FillVigorOnRandomize=1\n"
        << "FillFpOnRandomize=1\n";
}

bool LoadConfig(ModConfig& config)
{
    const std::string configPath =
        StatsRandomizerDir() + "BossStatsRandomizer.ini";

    std::ifstream file(configPath);

    if (!file.is_open())
    {
        Log("Config: BossStatsRandomizer.ini not found; "
            "generating a random seed and writing defaults.");

        config.globalSeed = GenerateRandomSeed();
        config.enableFogGateDetection = true;
        config.enableRuntimeApplication = true;
        config.enableBossDiagnostics = true;
        config.cacheInitialStats = true;
        config.randomizeWondrousPhysick = false;
        config.wondrousPhysickIncludeDlc = false;
        config.logLevel = 1;
        config.minAttribute = 1;
        config.maxAttribute = 99;

        for (int i = 0; i < PlayerOffsets::AttributeCount; ++i)
        {
            config.maxAttributeByIndex[i] = 99;
        }

        config.randomizeFlaskCharges = true;
        config.randomizeFlaskLevel = true;
        config.minFlaskCharges = 2;
        config.maxFlaskCharges = 14;
        config.minFlaskLevel = 1;
        config.maxFlaskLevel = 12;
        config.fillVigorOnRandomize = true;
        config.fillFpOnRandomize = true;
        config.showConsole = false;

        WriteConfigDefaults(config.globalSeed);

        Log("Config: wrote BossStatsRandomizer.ini with "
            "the new random GlobalSeed.");

        return true;
    }

    bool seedSeen = false;

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
        {
            config.globalSeed = ParseU64(value);
            seedSeen = true;
        }
        else if (key == "EnableFogGateDetection")
            config.enableFogGateDetection = (ParseU64(value) != 0);
        else if (key == "EnableRuntimeApplication")
            config.enableRuntimeApplication = (ParseU64(value) != 0);
        else if (key == "EnableBossDiagnostics")
            config.enableBossDiagnostics = (ParseU64(value) != 0);
        else if (key == "CacheInitialStats")
            config.cacheInitialStats = (ParseU64(value) != 0);
        else if (key == "RandomizeWondrousPhysick")
            config.randomizeWondrousPhysick = (ParseU64(value) != 0);
        else if (key == "WondrousPhysickIncludeDlc")
            config.wondrousPhysickIncludeDlc = (ParseU64(value) != 0);
        else if (key == "LogLevel")
            config.logLevel = static_cast<int>(ParseU64(value));
        else if (key == "MinAttribute")
            config.minAttribute = static_cast<int>(ParseU64(value));
        else if (key == "MaxAttribute")
            config.maxAttribute = static_cast<int>(ParseU64(value));
        else if (key.rfind("Max", 0) == 0)
        {
            int idx = AttributeIndexFromName(key.substr(3));

            if (idx >= 0)
            {
                int parsed = static_cast<int>(ParseU64(value));

                if (parsed >= 1 &&
                    parsed <= PlayerOffsets::MaxAttributeValue)
                {
                    config.maxAttributeByIndex[idx] = parsed;
                }
            }
        }
        else if (key == "RandomizeFlaskCharges")
            config.randomizeFlaskCharges = (ParseU64(value) != 0);
        else if (key == "RandomizeFlaskLevel")
            config.randomizeFlaskLevel = (ParseU64(value) != 0);
        else if (key == "MinFlaskCharges")
            config.minFlaskCharges = static_cast<int>(ParseU64(value));
        else if (key == "MaxFlaskCharges")
            config.maxFlaskCharges = static_cast<int>(ParseU64(value));
        else if (key == "MinFlaskLevel")
            config.minFlaskLevel = static_cast<int>(ParseU64(value));
        else if (key == "MaxFlaskLevel")
            config.maxFlaskLevel = static_cast<int>(ParseU64(value));
        else if (key == "FillVigorOnRandomize")
            config.fillVigorOnRandomize = (ParseU64(value) != 0);
        else if (key == "FillFpOnRandomize")
            config.fillFpOnRandomize = (ParseU64(value) != 0);
        else if (key == "ShowConsole")
            config.showConsole = (ParseU64(value) != 0);
    }

    if (!seedSeen)
    {
        Log("Config: GlobalSeed not present in ini; "
            "generating a new random seed.");

        config.globalSeed = GenerateRandomSeed();

        WriteConfigDefaults(config.globalSeed);

        Log("Config: rewrote BossStatsRandomizer.ini with "
            "the new random GlobalSeed.");
    }

    if (config.minAttribute < 1)
        config.minAttribute = 1;

    if (config.maxAttribute > PlayerOffsets::MaxAttributeValue)
        config.maxAttribute = PlayerOffsets::MaxAttributeValue;

    if (config.maxAttribute < config.minAttribute)
        config.maxAttribute = config.minAttribute;

    if (config.logLevel < 0)
        config.logLevel = 0;

    if (config.logLevel > 1)
        config.logLevel = 1;

    // Min/MaxFlaskCharges are a COMBINED (HP+FP) total, capped by the game's
    // 14-flask limit. MaxFlaskCharges is the combined cap; each flask type can
    // still end up anywhere within it.
    if (config.minFlaskCharges < 0)
        config.minFlaskCharges = 0;

    if (config.maxFlaskCharges > PlayerOffsets::MaxFlaskCharges)
        config.maxFlaskCharges = PlayerOffsets::MaxFlaskCharges;

    if (config.maxFlaskCharges < config.minFlaskCharges)
        config.maxFlaskCharges = config.minFlaskCharges;

    if (config.minFlaskLevel < 1)
        config.minFlaskLevel = 1;

    if (config.maxFlaskLevel > PlayerOffsets::MaxFlaskLevel)
        config.maxFlaskLevel = PlayerOffsets::MaxFlaskLevel;

    if (config.maxFlaskLevel < config.minFlaskLevel)
        config.maxFlaskLevel = config.minFlaskLevel;

    g_logLevel = config.logLevel;

    {
        std::ostringstream line;
        line << "Config loaded | GlobalSeed: "
            << std::dec
            << config.globalSeed
            << " | EnableFogGateDetection: "
            << (config.enableFogGateDetection ? "yes" : "no")
            << " | EnableRuntimeApplication: "
            << (config.enableRuntimeApplication ? "yes" : "no")
            << " | CacheInitialStats: "
            << (config.cacheInitialStats ? "yes" : "no")
            << " | RandomizeWondrousPhysick: "
            << (config.randomizeWondrousPhysick ? "yes" : "no")
            << (config.randomizeWondrousPhysick
                ? (config.wondrousPhysickIncludeDlc ? " (+SotE tears)" : " (base tears)")
                : "")
            << " | LogLevel: "
            << config.logLevel
            << " | EnableBossDiagnostics: "
            << (config.enableBossDiagnostics ? "yes" : "no")
            << " | Attribute range: "
            << config.minAttribute << ".." << config.maxAttribute;

        int caps[PlayerOffsets::AttributeCount] = { 0 };
        ResolveAttributeCaps(config, caps);

        line << " | Per-attribute caps: "
            << caps[0] << " "
            << caps[1] << " "
            << caps[2] << " "
            << caps[3] << " "
            << caps[4] << " "
            << caps[5] << " "
            << caps[6] << " "
            << caps[7]
            << " | Flask charges: "
            << (config.randomizeFlaskCharges ? "random " : "off ")
            << config.minFlaskCharges << ".." << config.maxFlaskCharges
            << " | Flask level: "
            << (config.randomizeFlaskLevel ? "random " : "off ")
            << config.minFlaskLevel << ".." << config.maxFlaskLevel
            << " | Fill on randomize: "
            << (config.fillVigorOnRandomize ? "vigor" : "")
            << (config.fillVigorOnRandomize && config.fillFpOnRandomize ? "+" : "")
            << (config.fillFpOnRandomize ? "fp" : "")
            << (config.fillVigorOnRandomize || config.fillFpOnRandomize ? " | " : "")
            << "Console: "
            << (config.showConsole ? "yes" : "no");

        Log(line.str());
    }

    return true;
}


// ============================================================
// Cached initial stats (BossStatsRandomizer.ini [CachedStats])
//
// The original base stats + physick tears are cached to the ini
// when a boss fight first randomizes them. If the game exits or
// crashes mid-fight, the mod restores them on the next launch.
// ============================================================

void WriteCachedStatsToIni(
    const uint32_t* baseStats,
    const uint32_t* physick,
    const SavedFlaskState& flask)
{
    const std::string configPath =
        StatsRandomizerDir() + "BossStatsRandomizer.ini";

    {
        std::ifstream check(configPath);
        std::string existing(
            (std::istreambuf_iterator<char>(check)),
            std::istreambuf_iterator<char>());

        if (existing.find("[CachedStats]") != std::string::npos)
            return;
    }

    std::ofstream file(configPath, std::ios::app);

    if (!file.is_open())
    {
        LogError("Config: could not write cached stats to "
            "BossStatsRandomizer.ini.");
        return;
    }

    file << "\n[CachedStats]\n";

    for (int i = 0; i < PlayerOffsets::AttributeCount; ++i)
    {
        file << "Initial" << (i + 1) << "=" << baseStats[i] << "\n";
    }

    for (int i = 0; i < PlayerOffsets::PhysickSlotCount; ++i)
    {
        file << "InitialPhysick" << (i + 1)
            << "=0x" << std::hex << physick[i] << std::dec << "\n";
    }

    file << "FlaskChargesHp=" << static_cast<int>(flask.hpCharges) << "\n"
        << "FlaskChargesFp=" << static_cast<int>(flask.fpCharges) << "\n"
        << "FlaskRateHp=0x" << std::hex
        << FloatToBits(flask.hpRate) << std::dec << "\n"
        << "FlaskAddHp=" << static_cast<int>(flask.hpAdditional) << "\n"
        << "FlaskRateFp=0x" << std::hex
        << FloatToBits(flask.fpRate) << std::dec << "\n"
        << "FlaskAddFp=" << static_cast<int>(flask.fpAdditional) << "\n";

    if (flask.bonfireLevel >= 1)
    {
        file << "FlaskBonfireLevel=" << flask.bonfireLevel << "\n";
    }
}

bool ReadCachedStatsFromIni(
    uint32_t* baseStats,
    uint32_t* physick,
    SavedFlaskState* flask,
    bool* flaskFound)
{
    const std::string configPath =
        StatsRandomizerDir() + "BossStatsRandomizer.ini";

    std::ifstream file(configPath);

    if (!file.is_open())
        return false;

    bool inSection = false;
    bool any = false;
    *flaskFound = false;

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

        if (trimmed[0] == '[')
        {
            inSection = (trimmed == "[CachedStats]");
            continue;
        }

        if (!inSection)
            continue;

        size_t equals = trimmed.find('=');

        if (equals == std::string::npos)
            continue;

        std::string key = Trim(trimmed.substr(0, equals));
        std::string value = Trim(trimmed.substr(equals + 1));

        if (key == "FlaskChargesHp")
        {
            flask->hpCharges = static_cast<uint8_t>(ParseU64(value));
            *flaskFound = true;
        }
        else if (key == "FlaskChargesFp")
        {
            flask->fpCharges = static_cast<uint8_t>(ParseU64(value));
            *flaskFound = true;
        }
        else if (key == "FlaskRateHp")
        {
            flask->hpRate = BitsToFloat(static_cast<uint32_t>(ParseU64(value)));
            *flaskFound = true;
        }
        else if (key == "FlaskAddHp")
        {
            flask->hpAdditional = static_cast<uint8_t>(ParseU64(value));
            *flaskFound = true;
        }
        else if (key == "FlaskRateFp")
        {
            flask->fpRate = BitsToFloat(static_cast<uint32_t>(ParseU64(value)));
            *flaskFound = true;
        }
        else if (key == "FlaskAddFp")
        {
            flask->fpAdditional = static_cast<uint8_t>(ParseU64(value));
            *flaskFound = true;
        }
        else if (key == "FlaskBonfireLevel")
        {
            flask->bonfireLevel = static_cast<int>(ParseU64(value));
            *flaskFound = true;
        }
        else if (key.rfind("InitialPhysick", 0) == 0)
        {
            int idx = atoi(key.c_str() + 14) - 1;

            if (idx >= 0 && idx < PlayerOffsets::PhysickSlotCount)
            {
                physick[idx] = static_cast<uint32_t>(ParseU64(value));
                any = true;
            }
        }
        else if (key.rfind("Initial", 0) == 0)
        {
            int idx = atoi(key.c_str() + 7) - 1;

            if (idx >= 0 && idx < PlayerOffsets::AttributeCount)
            {
                baseStats[idx] = static_cast<uint32_t>(ParseU64(value));
                any = true;
            }
        }
    }

    return any;
}

void ClearCachedStatsFromIni()
{
    const std::string configPath =
        StatsRandomizerDir() + "BossStatsRandomizer.ini";

    std::ifstream in(configPath);

    if (!in.is_open())
        return;

    std::stringstream kept;
    std::string line;
    bool skipping = false;

    while (std::getline(in, line))
    {
        std::string trimmed = Trim(line);

        if (trimmed == "[CachedStats]")
        {
            skipping = true;
            continue;
        }

        if (skipping)
        {
            if (trimmed[0] == '[')
            {
                skipping = false; // next section starts
            }
            else
            {
                continue;
            }
        }

        kept << line << '\n';
    }

    in.close();

    std::ofstream out(configPath, std::ios::trunc);

    if (!out.is_open())
        return;

    out << kept.str();
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
        *reinterpret_cast<unsigned char*>(
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
    //
    // The load_status enum (fromsoftware-rs v0.14.0, LIKELY) reads 3-4
    // ("network initializing"/"ready for activation") for a normal loaded
    // single-player character and never settles on Active(2), so it cannot
    // be the discriminator. The per-character flags is_active (chr_flags1c8
    // bit 4) + is_main_player are the meaningful "this is the loaded local
    // player" signals and are what we key on.
    // ------------------------------------------------------------------

    state.valid = true;

    if (!state.isActive)
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

    uint32_t previousCharacterEventId = 0;

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
        //
        // The GameDataMan-level boss_fight_active rising edge is itself an
        // authoritative fog-traversal signal (verified live: Margit toggles
        // it true on entry and false on defeat), and boss_npc_param_id
        // (0xCC) identifies the specific boss/gate. It therefore must NOT
        // require player.authoritative: during the fog crossing the player
        // can briefly leave the Active load state, which would make the
        // strict gate false (this is why no traversal was ever emitted).
        if (bossFightActive && !wasBossFightActive)
        {
            event.valid = true;

            event.gate.fogWallEntityId = 0; // requires event hook (NOT VERIFIED)
            event.gate.blockId = player.blockId;
            event.gate.playRegionId = player.playRegionId;
            event.gate.bossNpcParamId = bossNpcParamId;

            // Do NOT fold the raw player position into the gate identity for
            // the boss-activation trigger. Position here is read at an
            // arbitrary activation moment (may be unloaded/0,-1,-1), which
            // would break per-boss determinism. boss_npc_param_id is the
            // stable, unique per-boss discriminator. Position stays 0 in the
            // key for this trigger.
            event.destinationBlock = player.blockId;
            event.destinationRegion = player.playRegionId;

            std::ostringstream line;

            line << "Boss fight activation (fog gate trigger) detected"
                << " | boss_npc_param_id: "
                << bossNpcParamId
                << " | authoritative: "
                << (player.authoritative ? "yes" : "no");

            if (player.characterEventId != 0)
            {
                line << " | character_event_id: "
                    << player.characterEventId;
            }

            if (player.blockId != 0 || player.playRegionId != 0)
            {
                line << " | block: "
                    << player.blockId
                    << " | play_region: "
                    << player.playRegionId;
            }

            Log(line.str());
        }

        wasAuthoritative = player.authoritative;
        wasBossFightActive = bossFightActive;
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
    int physickApplied = 0;
    int flaskApplied = 0;
    bool accepted = false;
};

// Wondrous Physick tear slot offsets in PlayerGameData.
static constexpr uintptr_t PhysickOffsets[PlayerOffsets::PhysickSlotCount] = {
    PlayerOffsets::PhysickTear1,
    PlayerOffsets::PhysickTear2,
    PlayerOffsets::PhysickExtraTear,
};

// Level -> heal-potency mapping (LIKELY approximation). The game resolves
// flask power through EstusFlaskRecoveryParam indexed by bonfire level;
// this emulates the same monotonic increase with a bounded formula.
float FlaskRateForLevel(int level)
{
    float rate = 0.20f + static_cast<float>(level) * 0.06f;
    return rate > 0.98f ? 0.98f : rate;
}

uint8_t FlaskAdditionalForLevel(int level)
{
    int additional = level * 3;
    return additional > 255 ? 255 : static_cast<uint8_t>(additional);
}

// ============================================================
// Flask level (item-id + bonfire-level byte, TGA "Set flask level")
//
// The in-game flask "+X" value is encoded in the flask ITEM ids and a
// "total bonfire level" byte (flask level + 1). This mirrors the TGA script
// (ReplaceFlasks then ActivateBonfire) but deliberately skips SaveRequest so
// the change is memory-only and gets reversed when the boss fight ends.
// ============================================================

constexpr int kFlaskItemCrimsonBase = 1000;    // Crimson Tears: 1000 + level*2
constexpr int kFlaskItemCeruleanBase = 1001;   // Cerulean Tears: 1001 + level*2
constexpr int kFlaskItemPhysickBase = 1050;    // Wondrous Physick item variants 1050/1051

int ReadTotalBonfireLevel()
{
    if (!g_totalBonfireLevelAddr)
    {
        return 0;
    }

    return *reinterpret_cast<uint8_t*>(g_totalBonfireLevelAddr);
}

void ExecuteActivateBonfire(int bonfireLevel)
{
    if (!g_activateBonfire ||
        bonfireLevel < 1 ||
        bonfireLevel > 13)
    {
        return;
    }

    typedef void(__fastcall* ActivateBonfireFn)(int);

    ActivateBonfireFn activate =
        reinterpret_cast<ActivateBonfireFn>(g_activateBonfire);

    activate(bonfireLevel);
}

void ExecuteReplaceTool(
    uintptr_t equipGameData,
    int currentId,
    int replaceId)
{
    if (!g_replaceTool || !equipGameData)
    {
        return;
    }

    // executeCodeEx(0, 100, addr, EquipGameData, currentId, replaceId, 1)
    typedef void(__fastcall* ReplaceToolFn)(
        uintptr_t, int, int, int);

    ReplaceToolFn replace =
        reinterpret_cast<ReplaceToolFn>(g_replaceTool);

    replace(equipGameData, currentId, replaceId, 1);
}

uintptr_t EquipGameDataFor(uintptr_t playerGameData)
{
    if (!playerGameData)
    {
        return 0;
    }

    return *reinterpret_cast<uintptr_t*>(
        playerGameData + PlayerOffsets::EquipGameDataOffset);
}

// Swaps the four flask item ids from the level encoded by currentBonfireLevel
// to newBonfireLevel (both 1-based total bonfire levels), then sets the
// bonfire-level byte. Order matches TGA (ReplaceFlasks before ActivateBonfire)
// so the swap can read the "old" level out of the byte.
void UpdateFlaskLevel(
    uintptr_t playerGameData,
    int currentBonfireLevel,
    int newBonfireLevel)
{
    if (!g_flaskLevelReady || !playerGameData)
    {
        return;
    }

    if (currentBonfireLevel < 1 || currentBonfireLevel > 13 ||
        newBonfireLevel < 1 || newBonfireLevel > 13)
    {
        return;
    }

    uintptr_t equipGameData = EquipGameDataFor(playerGameData);

    if (!equipGameData)
    {
        return;
    }

    const int flaskBases[4] = {
        kFlaskItemCrimsonBase,
        kFlaskItemCeruleanBase,
        kFlaskItemPhysickBase,
        kFlaskItemPhysickBase + 1,
    };

    // oldItem/newItem are the item ids the current/next flask level maps to.
    for (int i = 0; i < 4; ++i)
    {
        int newItem = flaskBases[i] + (newBonfireLevel - 1) * 2;
        int oldItem = flaskBases[i] + (currentBonfireLevel - 1) * 2;

        if (oldItem != newItem)
        {
            ExecuteReplaceTool(equipGameData, oldItem, newItem);
        }
    }

    ExecuteActivateBonfire(newBonfireLevel);
}

// ============================================================
// Wondrous Physick: full crystal-tear pool + deterministic pick
//
// Tear item IDs come from TGA's "Give all crystal tears" scripts (base game
// and Shadow of the Erdtree), authoritative for ER 1.16.2.
// ============================================================

constexpr uint32_t kBaseTearFirst = 0x40002AF8u; // Crimsonspill Crystal Tear
constexpr uint32_t kBaseTearLast  = 0x40002B17u; // Holy-Shrouding Cracked Tear
constexpr int kBaseTearCount      = 32;

const uint32_t kSoteTearIds[] = {
    0x401EAF78u, // Viridian Hidden Tear
    0x401EAF82u, // Crimsonburst Dried Tear
    0x401EAF8Cu, // Crimson-Sapping Cracked Tear
    0x401EAF96u, // Cerulean-Sapping Cracked Tear
    0x401EAFA0u, // Oil-Soaked Tear
    0x401EAFAAu, // Bloodsucking Cracked Tear
    0x401EAFB4u, // Glovewort Crystal Tear
    0x401EAFBEu, // Deflecting Hardtear
};
constexpr int kSoteTearCount = 8;
constexpr int kMaxTearPoolCount = kBaseTearCount + kSoteTearCount;

// Builds the tear pool into `pool` and sets `count`. Base-game tears are
// always included; SotE tears only when `includeDlc` is set.
void BuildPhysickTearPool(bool includeDlc, uint32_t* pool, int& count)
{
    count = 0;

    for (uint32_t id = kBaseTearFirst; id <= kBaseTearLast; ++id)
    {
        pool[count++] = id;
    }

    if (includeDlc)
    {
        for (int i = 0; i < kSoteTearCount; ++i)
        {
            pool[count++] = kSoteTearIds[i];
        }
    }
}

// Deterministic full-pool pick: PhysickSlotCount distinct tears sampled from
// the whole mixable pool, seeded by gateSeed. Same gate -> same loadout on
// every encounter (and every launch), independent of what the player had
// equipped. The pool always holds >= 32 tears, so the two mix slots + the
// tertiary slot are always drawn without replacement.
bool PickPhysickTears(
    uint64_t gateSeed,
    bool includeDlc,
    uint32_t outTears[PlayerOffsets::PhysickSlotCount])
{
    uint32_t pool[kMaxTearPoolCount];
    int poolCount = 0;
    BuildPhysickTearPool(includeDlc, pool, poolCount);

    if (poolCount <= 0)
    {
        return false;
    }

    SeededRandom rng(DeterministicHash::Mix(gateSeed, 0xF1A6ULL));

    // Full Fisher-Yates (pool is tiny, < 40 entries) so the first N entries
    // are a uniform, duplicate-free sample.
    for (int i = poolCount - 1; i > 0; --i)
    {
        uint64_t j = rng.NextU64() % (static_cast<uint64_t>(i) + 1ULL);

        uint32_t tmp = pool[i];
        pool[i] = pool[static_cast<int>(j)];
        pool[static_cast<int>(j)] = tmp;
    }

    int take = poolCount < PlayerOffsets::PhysickSlotCount
        ? poolCount
        : PlayerOffsets::PhysickSlotCount;

    for (int i = 0; i < take; ++i)
    {
        outTears[i] = pool[i];
    }

    return take == PlayerOffsets::PhysickSlotCount;
}

// ============================================================
// Flask charges
// ============================================================

// Writes the allocated flask charge counts. The byte fields (PlayerGameData+
// 0x101/0x102, VERIFIED) are the "allocated at grace" persistants; the engine
// update routine below additionally applies them to the live flask counters so
// the change is visible immediately during the fight (same technique as TGA's
// "Add charge to flask" script).
void WriteFlaskCharges(
    uintptr_t playerGameData,
    uint8_t hpCharges,
    uint8_t fpCharges)
{
    if (!playerGameData)
    {
        return;
    }

    *reinterpret_cast<uint8_t*>(
        playerGameData + PlayerOffsets::MaxHpFlask) = hpCharges;
    *reinterpret_cast<uint8_t*>(
        playerGameData + PlayerOffsets::MaxFpFlask) = fpCharges;

    if (!g_estusUpdateReady)
    {
        return;
    }

    // Signature: void(int flaskType, int count). 0 = HP flask, 1 = FP flask.
    typedef int(__fastcall* EstusAllocationUpdateFn)(int, int);

    EstusAllocationUpdateFn apply =
        reinterpret_cast<EstusAllocationUpdateFn>(g_estusAllocationUpdate);

    apply(0, static_cast<int>(hpCharges));
    apply(1, static_cast<int>(fpCharges));
}

uintptr_t StatBaseForGameDataMan(uintptr_t gameDataMan)
{
    if (!gameDataMan)
        return 0;

    return *reinterpret_cast<uintptr_t*>(
        gameDataMan + PlayerOffsets::StatBasePtr);
}

// Tops the player's HP/FP up to their current maxima. Writes both the
// PlayerGameData fields (VERIFIED 0x10/0x14, 0x1C/0x20) and, when the player
// ChrIns pointer is supplied, the live character module (PlayerIns + 0x190 ->
// ChrInsModuleContainer + 0x00 -> CSChrDataModule, hp 0x138 / fp 0x148). The
// module fields are what the HUD/fight actually reads.
void FillPlayerResources(
    uintptr_t playerGameData,
    uintptr_t chrIns,
    bool fillVigor,
    bool fillFp)
{
    if (fillVigor)
    {
        uint32_t maxHp = 0;

        if (playerGameData)
        {
            maxHp = *reinterpret_cast<uint32_t*>(
                playerGameData + PlayerOffsets::CurrentMaxHp);

            if (maxHp > 0)
            {
                *reinterpret_cast<uint32_t*>(
                    playerGameData + PlayerOffsets::CurrentHp) = maxHp;
            }
        }

        if (chrIns)
        {
            uintptr_t container = *reinterpret_cast<uintptr_t*>(
                chrIns + PlayerOffsets::ChrInsModules);

            uintptr_t dataModule = container
                ? *reinterpret_cast<uintptr_t*>(
                    container + PlayerOffsets::ChrInsModuleContainerData)
                : 0;

            if (dataModule)
            {
                uint32_t moduleMaxHp = *reinterpret_cast<uint32_t*>(
                    dataModule + PlayerOffsets::ChrDataModuleMaxHp);

                if (moduleMaxHp > 0)
                {
                    *reinterpret_cast<uint32_t*>(
                        dataModule + PlayerOffsets::ChrDataModuleHp) = moduleMaxHp;
                }
            }
        }
    }

    if (fillFp)
    {
        uint32_t maxFp = 0;

        if (playerGameData)
        {
            maxFp = *reinterpret_cast<uint32_t*>(
                playerGameData + PlayerOffsets::CurrentMaxFp);

            if (maxFp > 0)
            {
                *reinterpret_cast<uint32_t*>(
                    playerGameData + PlayerOffsets::CurrentFp) = maxFp;
            }
        }

        if (chrIns)
        {
            uintptr_t container = *reinterpret_cast<uintptr_t*>(
                chrIns + PlayerOffsets::ChrInsModules);

            uintptr_t dataModule = container
                ? *reinterpret_cast<uintptr_t*>(
                    container + PlayerOffsets::ChrInsModuleContainerData)
                : 0;

            if (dataModule)
            {
                uint32_t moduleMaxFp = *reinterpret_cast<uint32_t*>(
                    dataModule + PlayerOffsets::ChrDataModuleMaxFp);

                if (moduleMaxFp > 0)
                {
                    *reinterpret_cast<uint32_t*>(
                        dataModule + PlayerOffsets::ChrDataModuleFp) = moduleMaxFp;
                }
            }
        }
    }
}

// Writes the deterministic attribute set to the PlayerGameData base
// attribute fields (0x3C-0x58, VERIFIED). The game recomputes the
// "effective" fields (0x288-0x2A8) from these base values every frame,
// so writing the base fields is what makes the change appear on the stat
// screen / character sheet.
//
// When CacheInitialStats is on, originals are captured into
// SavedBaseStats/SavedPhysick/SavedFlask before the first write so they
// can be restored when the boss fight ends.
//
// When RandomizeWondrousPhysick is on, each Wondrous Physick mix slot is
// filled with a deterministic random tear drawn from the full mixable pool
// (base game + optional Shadow of the Erdtree), seeded by this gate. Active
// picks are returned in activePhysick so the fight loop can re-apply them.
//
// When RandomizeFlaskCharges / RandomizeFlaskLevel are on, the flask
// charge counts (combined total, applied through the engine's own update
// routine so they are visible immediately) and heal potency fields are
// randomized within the config ranges, deterministically seeded from this
// gate. Randomized potency is returned in activeFlask for re-application.
GateApplicationResult ApplyGateResult(
    const ActivePlayerState& player,
    const GateAttributeResult& gateResult,
    const ModConfig& config,
    const int* maxAttributeByIndex,
    uint32_t* savedBaseStats,
    bool* haveSavedBaseStats,
    uint32_t* savedPhysick,
    bool* haveSavedPhysick,
    SavedFlaskState* savedFlask,
    bool* haveSavedFlask,
    uint32_t* activePhysick,
    bool* haveActivePhysick,
    SavedFlaskState* activeFlask,
    bool* haveActiveFlask,
    uint64_t* fillUntilTick)
{
    GateApplicationResult outcome;
    outcome.accepted = false;

    if (!config.enableRuntimeApplication)
    {
        return outcome;
    }

    if (!player.authoritative || !player.playerGameData)
    {
        outcome.rejected = PlayerOffsets::AttributeCount;
        return outcome;
    }

    if (config.cacheInitialStats && !*haveSavedBaseStats)
    {
        for (int i = 0; i < PlayerOffsets::AttributeCount; ++i)
        {
            savedBaseStats[i] =
                *reinterpret_cast<uint32_t*>(
                    player.playerGameData + PlayerOffsets::AttributeOffsets[i]);
        }

        for (int i = 0; i < PlayerOffsets::PhysickSlotCount; ++i)
        {
            savedPhysick[i] =
                *reinterpret_cast<uint32_t*>(
                    player.playerGameData + PhysickOffsets[i]);
        }

        savedFlask->hpCharges = *reinterpret_cast<uint8_t*>(
            player.playerGameData + PlayerOffsets::MaxHpFlask);
        savedFlask->fpCharges = *reinterpret_cast<uint8_t*>(
            player.playerGameData + PlayerOffsets::MaxFpFlask);
        savedFlask->hpRate = *reinterpret_cast<float*>(
            player.playerGameData + PlayerOffsets::HpEstusRate);
        savedFlask->hpAdditional = *reinterpret_cast<uint8_t*>(
            player.playerGameData + PlayerOffsets::HpEstusAdditional);
        savedFlask->fpRate = *reinterpret_cast<float*>(
            player.playerGameData + PlayerOffsets::FpEstusRate);
        savedFlask->fpAdditional = *reinterpret_cast<uint8_t*>(
            player.playerGameData + PlayerOffsets::FpEstusAdditional);
        savedFlask->bonfireLevel = ReadTotalBonfireLevel();

        *haveSavedBaseStats = true;
        *haveSavedPhysick = true;
        *haveSavedFlask = true;

        // Persist the originals so a crash/quit mid-fight can be
        // recovered on the next launch.
        WriteCachedStatsToIni(savedBaseStats, savedPhysick, *savedFlask);
    }

    for (int i = 0; i < PlayerOffsets::AttributeCount; ++i)
    {
        int value = gateResult.attributes[i];

        if (value < config.minAttribute ||
            value > maxAttributeByIndex[i])
        {
            ++outcome.rejected;
            continue;
        }

        *reinterpret_cast<uint32_t*>(
            player.playerGameData + PlayerOffsets::AttributeOffsets[i]
            ) = static_cast<uint32_t>(value);

        ++outcome.applied;
    }

    if (config.randomizeWondrousPhysick)
    {
        // Full-pool pick (not a permutation of what the player happened to
        // have equipped): every mix slot gets a deterministic random tear from
        // all mixable tears (base game + optional SotE).
        uint32_t randomTears[PlayerOffsets::PhysickSlotCount] = { 0 };

        if (PickPhysickTears(
            gateResult.gateSeed,
            config.wondrousPhysickIncludeDlc,
            randomTears))
        {
            for (int i = 0; i < PlayerOffsets::PhysickSlotCount; ++i)
            {
                *reinterpret_cast<uint32_t*>(
                    player.playerGameData + PhysickOffsets[i]
                    ) = randomTears[i];

                activePhysick[i] = randomTears[i];
            }

            *haveActivePhysick = true;
            outcome.physickApplied = PlayerOffsets::PhysickSlotCount;
        }
    }

    // Flask charges + heal potency, deterministically seeded per gate.
    if (config.randomizeFlaskCharges || config.randomizeFlaskLevel)
    {
        SeededRandom flaskRng(
            DeterministicHash::Mix(gateResult.gateSeed, 0xF1A5ULL));

        if (config.randomizeFlaskCharges)
        {
            // Charges are a COMBINED total rolled in [min..max]; the total is
            // then split into Crimson + Cerulean (the game caps at 14 total).
            int totalCharges = flaskRng.NextBounded(
                config.minFlaskCharges, config.maxFlaskCharges);
            int hpCharges = flaskRng.NextBounded(0, totalCharges);
            int fpCharges = totalCharges - hpCharges;

            WriteFlaskCharges(
                player.playerGameData,
                static_cast<uint8_t>(hpCharges),
                static_cast<uint8_t>(fpCharges));

            outcome.flaskApplied += 2;
        }

        if (config.randomizeFlaskLevel)
        {
            // The game has exactly ONE flask upgrade level (sacred tears
            // raise all four flask items together), so roll a single level
            // and drive both the potency fields and the item/bonfire-level
            // swap from it.
            int level = flaskRng.NextBounded(
                config.minFlaskLevel, config.maxFlaskLevel);

            activeFlask->hpRate = FlaskRateForLevel(level);
            activeFlask->hpAdditional = FlaskAdditionalForLevel(level);
            activeFlask->fpRate = FlaskRateForLevel(level);
            activeFlask->fpAdditional = FlaskAdditionalForLevel(level);
            *haveActiveFlask = true;

            *reinterpret_cast<float*>(
                player.playerGameData + PlayerOffsets::HpEstusRate)
                = activeFlask->hpRate;
            *reinterpret_cast<uint8_t*>(
                player.playerGameData + PlayerOffsets::HpEstusAdditional)
                = activeFlask->hpAdditional;
            *reinterpret_cast<float*>(
                player.playerGameData + PlayerOffsets::FpEstusRate)
                = activeFlask->fpRate;
            *reinterpret_cast<uint8_t*>(
                player.playerGameData + PlayerOffsets::FpEstusAdditional)
                = activeFlask->fpAdditional;

            outcome.flaskApplied += 4;

            // Swap the flask item ids to the rolled level and set the
            // bonfire-level byte so the "+X" and actual heal scale live.
            // Skipped if the AOBs didn't resolve or we never saw a valid
            // starting level (fresh save without a flask yet, etc.).
            if (g_flaskLevelReady &&
                savedFlask->bonfireLevel >= 1 &&
                savedFlask->bonfireLevel <= 13)
            {
                int newBonfireLevel = level + 1; // total bonfire level = +1

                UpdateFlaskLevel(
                    player.playerGameData,
                    savedFlask->bonfireLevel,
                    newBonfireLevel);

                activeFlask->bonfireLevel = newBonfireLevel;
                outcome.flaskApplied += 2;
            }
        }
    }

    // Top off HP/FP so the randomization doesn't leave the player at a
    // fresh-out-of-a-fight deficit, then keep topping up for the whole fight
    // (the fight loop below re-applies the fill every poll) so the refill
    // can't be wiped by the engine's own recalculation.
    FillPlayerResources(
        player.playerGameData,
        player.mainPlayer,
        config.fillVigorOnRandomize,
        config.fillFpOnRandomize);

    if (config.fillVigorOnRandomize || config.fillFpOnRandomize)
    {
        constexpr uint64_t kFillSettleMilliseconds = 3000;
        *fillUntilTick = GetTickCount64() + kFillSettleMilliseconds;
    }

    outcome.accepted = (outcome.rejected == 0);
    return outcome;
}


// Restores the base attribute fields captured when the gate result was
// applied. Uses the GameDataMan StatBase pointer (which stays valid even
// when the WorldChrMan active-player chain tears down on death/load), and
// is a no-op if the pointer is unavailable.
bool RestoreSavedBaseStats(
    uintptr_t gameDataMan,
    const uint32_t* savedBaseStats,
    bool haveSavedBaseStats)
{
    if (!haveSavedBaseStats)
    {
        return false;
    }

    if (!gameDataMan)
    {
        return false;
    }

    uintptr_t statBase =
        *reinterpret_cast<uintptr_t*>(
            gameDataMan + PlayerOffsets::StatBasePtr
            );

    if (!statBase)
    {
        return false;
    }

    for (int i = 0; i < PlayerOffsets::AttributeCount; ++i)
    {
        *reinterpret_cast<uint32_t*>(
            statBase + PlayerOffsets::AttributeOffsets[i]
            ) = savedBaseStats[i];
    }

    return true;
}

// Restores the Wondrous Physick tear slots captured when the gate result
// was applied (see RestoreSavedBaseStats for the statBase rationale).
bool RestoreSavedPhysick(
    uintptr_t gameDataMan,
    const uint32_t* savedPhysick,
    bool haveSavedPhysick)
{
    if (!haveSavedPhysick)
    {
        return false;
    }

    if (!gameDataMan)
    {
        return false;
    }

    uintptr_t statBase =
        *reinterpret_cast<uintptr_t*>(
            gameDataMan + PlayerOffsets::StatBasePtr
            );

    if (!statBase)
    {
        return false;
    }

    for (int i = 0; i < PlayerOffsets::PhysickSlotCount; ++i)
    {
        *reinterpret_cast<uint32_t*>(
            statBase + PhysickOffsets[i]
            ) = savedPhysick[i];
    }

    return true;
}

// Restores the flask charge/heal-potency fields captured when the gate
// result was applied (see RestoreSavedBaseStats for the statBase rationale).
bool RestoreSavedFlask(
    uintptr_t gameDataMan,
    const SavedFlaskState& savedFlask,
    bool haveSavedFlask)
{
    if (!haveSavedFlask)
    {
        return false;
    }

    uintptr_t statBase = StatBaseForGameDataMan(gameDataMan);

    if (!statBase)
    {
        return false;
    }

    *reinterpret_cast<uint8_t*>(
        statBase + PlayerOffsets::MaxHpFlask) = savedFlask.hpCharges;
    *reinterpret_cast<uint8_t*>(
        statBase + PlayerOffsets::MaxFpFlask) = savedFlask.fpCharges;
    *reinterpret_cast<float*>(
        statBase + PlayerOffsets::HpEstusRate) = savedFlask.hpRate;
    *reinterpret_cast<uint8_t*>(
        statBase + PlayerOffsets::HpEstusAdditional) = savedFlask.hpAdditional;
    *reinterpret_cast<float*>(
        statBase + PlayerOffsets::FpEstusRate) = savedFlask.fpRate;
    *reinterpret_cast<uint8_t*>(
        statBase + PlayerOffsets::FpEstusAdditional) = savedFlask.fpAdditional;

    // Swap the flask item ids back to the capture-time level and restore the
    // bonfire-level byte (reverse of UpdateFlaskLevel on apply).
    if (savedFlask.bonfireLevel >= 1 && savedFlask.bonfireLevel <= 13)
    {
        int currentBonfireLevel = ReadTotalBonfireLevel();

        if (currentBonfireLevel >= 1 &&
            currentBonfireLevel <= 13 &&
            currentBonfireLevel != savedFlask.bonfireLevel)
        {
            UpdateFlaskLevel(
                statBase,
                currentBonfireLevel,
                savedFlask.bonfireLevel);
        }
    }

    return true;
}

// Applies the [CachedStats] originals to the active player and returns true
// only once every written value is confirmed settled via read-back. The
// startup crash recovery calls this repeatedly while the character load-in can
// still be overwriting our writes; the cache is cleared by the caller only
// after this reports success.
bool ApplyCachedRestore(
    uintptr_t playerGameData,
    const uint32_t* cachedStats,
    const uint32_t* cachedPhysick,
    const SavedFlaskState& cachedFlask,
    bool cachedFlaskFound,
    bool fillVigor,
    bool fillFp)
{
    if (!playerGameData)
    {
        return false;
    }

    for (int i = 0; i < PlayerOffsets::AttributeCount; ++i)
    {
        *reinterpret_cast<uint32_t*>(
            playerGameData + PlayerOffsets::AttributeOffsets[i]
            ) = cachedStats[i];
    }

    for (int i = 0; i < PlayerOffsets::PhysickSlotCount; ++i)
    {
        uint32_t current = *reinterpret_cast<uint32_t*>(
            playerGameData + PhysickOffsets[i]);

        if (cachedPhysick[i] != 0 || current != 0)
        {
            *reinterpret_cast<uint32_t*>(
                playerGameData + PhysickOffsets[i]
                ) = cachedPhysick[i];
        }
    }

    if (cachedFlaskFound)
    {
        *reinterpret_cast<uint8_t*>(
            playerGameData + PlayerOffsets::MaxHpFlask) = cachedFlask.hpCharges;
        *reinterpret_cast<uint8_t*>(
            playerGameData + PlayerOffsets::MaxFpFlask) = cachedFlask.fpCharges;
        *reinterpret_cast<float*>(
            playerGameData + PlayerOffsets::HpEstusRate) = cachedFlask.hpRate;
        *reinterpret_cast<uint8_t*>(
            playerGameData + PlayerOffsets::HpEstusAdditional)
            = cachedFlask.hpAdditional;
        *reinterpret_cast<float*>(
            playerGameData + PlayerOffsets::FpEstusRate) = cachedFlask.fpRate;
        *reinterpret_cast<uint8_t*>(
            playerGameData + PlayerOffsets::FpEstusAdditional)
            = cachedFlask.fpAdditional;
    }

    // A crash mid-fight may have left the flask item ids / bonfire byte on
    // the randomized level; put them back on the cached original level.
    if (cachedFlaskFound &&
        cachedFlask.bonfireLevel >= 1 &&
        cachedFlask.bonfireLevel <= 13)
    {
        int currentBonfireLevel = ReadTotalBonfireLevel();

        if (currentBonfireLevel >= 1 &&
            currentBonfireLevel <= 13 &&
            currentBonfireLevel != cachedFlask.bonfireLevel)
        {
            UpdateFlaskLevel(
                playerGameData,
                currentBonfireLevel,
                cachedFlask.bonfireLevel);
        }
    }

    FillPlayerResources(
        playerGameData,
        0,
        fillVigor,
        fillFp);

    // Verify every entry we affected settled in memory.
    for (int i = 0; i < PlayerOffsets::AttributeCount; ++i)
    {
        if (*reinterpret_cast<uint32_t*>(
            playerGameData + PlayerOffsets::AttributeOffsets[i]) != cachedStats[i])
        {
            return false;
        }
    }

    for (int i = 0; i < PlayerOffsets::PhysickSlotCount; ++i)
    {
        if (*reinterpret_cast<uint32_t*>(
            playerGameData + PhysickOffsets[i]) != cachedPhysick[i])
        {
            return false;
        }
    }

    if (cachedFlaskFound)
    {
        if (*reinterpret_cast<uint8_t*>(
            playerGameData + PlayerOffsets::MaxHpFlask) != cachedFlask.hpCharges)
            return false;
        if (*reinterpret_cast<uint8_t*>(
            playerGameData + PlayerOffsets::MaxFpFlask) != cachedFlask.fpCharges)
            return false;
        if (*reinterpret_cast<float*>(
            playerGameData + PlayerOffsets::HpEstusRate) != cachedFlask.hpRate)
            return false;
        if (*reinterpret_cast<uint8_t*>(
            playerGameData + PlayerOffsets::HpEstusAdditional) != cachedFlask.hpAdditional)
            return false;
        if (*reinterpret_cast<float*>(
            playerGameData + PlayerOffsets::FpEstusRate) != cachedFlask.fpRate)
            return false;
        if (*reinterpret_cast<uint8_t*>(
            playerGameData + PlayerOffsets::FpEstusAdditional) != cachedFlask.fpAdditional)
            return false;
        if (cachedFlask.bonfireLevel >= 1 &&
            cachedFlask.bonfireLevel <= 13 &&
            ReadTotalBonfireLevel() != cachedFlask.bonfireLevel)
            return false;
    }

    return true;
}


// ============================================================
// Main runtime
// ============================================================

DWORD WINAPI MainThread(LPVOID)
{
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
    Log("Boss Stats Randomizer initialized");

    Log("Target App: " +
        std::string(Game::ExpectedAppVersion));

    Log("Target Calibration: " +
        std::string(Game::ExpectedCalibrationVersion));

    Log("========================================");

    ModConfig config;
    LoadConfig(config);

    // Console is opt-in (ShowConsole, default 0). Attach it only after the
    // config is loaded so the headless default never flashes a window.
    if (config.showConsole)
    {
        EnsureConsole();
    }

    if (config.enableRuntimeApplication)
    {
        Log("WARNING: EnableRuntimeApplication is ON. "
            "Base stat fields are temporarily randomized during boss "
            "fights and restored afterwards.");
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

    // --------------------------------------------------------
    // Resolve the engine's flask-charge update routine (optional).
    // Used so randomized flask charges take effect immediately instead of
    // only on the next rest. Falls back to byte-writes if not found.
    // --------------------------------------------------------

    g_estusAllocationUpdate =
        Game::FindPattern(
            ESTUS_ALLOC_UPDATE_PATTERN,
            ESTUS_ALLOC_UPDATE_MASK
        );

    g_estusUpdateReady = (g_estusAllocationUpdate != 0);

    if (g_estusUpdateReady)
    {
        std::ostringstream estusAddress;

        estusAddress << "EstusAllocationUpdate: 0x"
            << std::hex << std::uppercase
            << g_estusAllocationUpdate;

        Log(estusAddress.str());
    }
    else
    {
        Log("WARNING: EstusAllocationUpdate signature not found; "
            "flask charges are byte-writes only and apply at the next rest.");
    }

    // --------------------------------------------------------
    // Resolve the flask-level (item-id + bonfire byte) routines (optional).
    // Enables the "+X" flask upgrade during a fight, matching TGA's
    // "Set flask level" technique. Falls back to potency-field-only writes if
    // either signature is missing.
    // --------------------------------------------------------

    g_activateBonfire =
        Game::FindPattern(
            BONFIRE_LEVEL_SET_PATTERN,
            BONFIRE_LEVEL_SET_MASK
        );

    g_replaceTool =
        Game::FindPattern(
            REPLACE_TOOL_PATTERN,
            REPLACE_TOOL_MASK
        );

    if (g_activateBonfire && g_replaceTool)
    {
        g_replaceTool = g_replaceTool - 0x19;

        // Total bonfire level byte = ActivateBonfire + 0x44 + rel32(+2) + 5.
        int32_t bonfireDisp =
            *reinterpret_cast<int32_t*>(g_activateBonfire + 0x44 + 2);

        g_totalBonfireLevelAddr =
            g_activateBonfire + 0x44 + bonfireDisp + 5;

        g_flaskLevelReady = true;

        std::ostringstream flaskLevel;
        flaskLevel << "Flask level routines resolved"
            << " | ActivateBonfire: 0x"
            << std::hex << std::uppercase << g_activateBonfire
            << " | ReplaceTool: 0x" << g_replaceTool
            << " | BonfireLevel byte: 0x"
            << std::hex << std::uppercase << g_totalBonfireLevelAddr
            << std::dec;

        Log(flaskLevel.str());
    }
    else
    {
        g_flaskLevelReady = false;

        Log("WARNING: Flask-level routine signature not found; "
            "flask level is potency-fields only (no item/bonfire swap).");
    }

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

    // Original base stats captured when a gate result is first applied, so
    // they can be restored when the boss fight ends. Writing base stats is
    // the only way the change is visible (the effective fields at 0x288+ are
    // recomputed from these every frame).
    uint32_t savedBaseStats[PlayerOffsets::AttributeCount] = { 0 };
    bool haveSavedBaseStats = false;

    // Original Wondrous Physick tears captured alongside the base stats.
    uint32_t savedPhysick[PlayerOffsets::PhysickSlotCount] = { 0 };
    bool haveSavedPhysick = false;

    // Original flask state captured alongside the base stats.
    SavedFlaskState savedFlask;
    bool haveSavedFlask = false;

    // What is ACTIVELY applied for the current fight (randomized picks), so
    // the monitor loop can re-apply it every iteration. The engine can
    // overwrite the physick loadout / estus potency after the one-shot write,
    // and continuous re-application makes the randomization stick.
    uint32_t activePhysick[PlayerOffsets::PhysickSlotCount] = { 0 };
    bool haveActivePhysick = false;
    SavedFlaskState activeFlask;
    bool haveActiveFlask = false;

    // HP/FP top-up settle window: the game recalculates the resource fields
    // on the frame after our write, so we keep topping up briefly to make the
    // refill visibly stick. 0 = no window active.
    uint64_t fillUntilTick = 0;

    // Startup crash recovery: restore any [CachedStats] left in the ini by a
    // previous session that ended mid-boss-fight. Kept retrying until the
    // restored values are actually settled in memory (the player can still be
    // loading when the first write lands), then the cache is cleared.
    bool cacheRestoreDone = false;
    bool cacheRestoreWarned = false;
    int cacheRestoreAttempts = 0;

    // Authority sub-state diagnostics (reveals which gate condition fails).
    bool haveAuthorityDiag = false;
    uintptr_t previousDiagMainPlayer = 0;
    unsigned int previousDiagLoadStatus = 0;
    bool previousDiagIsActive = false;
    bool previousDiagIsMainPlayer = false;

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

        // Diagnostic: reveal which sub-condition of the authoritative gate
        // (load_status / is_active / is_main_player, all LIKELY offsets)
        // is failing. Logged on any change so we can validate those offsets
        // at runtime.
        if (config.enableBossDiagnostics)
        {
            if (!haveAuthorityDiag ||
                player.loadStatus != previousDiagLoadStatus ||
                player.isActive != previousDiagIsActive ||
                player.isMainPlayer != previousDiagIsMainPlayer ||
                player.mainPlayer != previousDiagMainPlayer)
            {
                std::ostringstream diag;

                diag << "AUTH_DIAG main_player: 0x"
                    << std::hex << std::uppercase
                    << player.mainPlayer
                    << " | load_status: "
                    << std::dec
                    << player.loadStatus
                    << " | is_active: "
                    << (player.isActive ? "true" : "false")
                    << " | is_main_player: "
                    << (player.isMainPlayer ? "true" : "false")
                    << " | player_game_data: 0x"
                    << std::hex << std::uppercase
                    << player.playerGameData
                    << std::dec;

                Log(diag.str());
            }

            haveAuthorityDiag = true;
            previousDiagLoadStatus = player.loadStatus;
            previousDiagIsActive = player.isActive;
            previousDiagIsMainPlayer = player.isMainPlayer;
            previousDiagMainPlayer = player.mainPlayer;
        }

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

        // ----------------------------------------------------
        // While a boss fight is active, re-apply the randomized loadout
        // (physick tears, estus potency) and keep HP/FP topped up for the
        // whole encounter. The engine recomputes these fields on its own
        // frames, so echoing them back keeps the randomization from being
        // silently reverted after the one-shot apply.
        // ----------------------------------------------------
        if (bossFightActive && player.authoritative && player.playerGameData)
        {
            if (haveActivePhysick)
            {
                for (int i = 0; i < PlayerOffsets::PhysickSlotCount; ++i)
                {
                    *reinterpret_cast<uint32_t*>(
                        player.playerGameData + PhysickOffsets[i]
                        ) = activePhysick[i];
                }
            }

            if (haveActiveFlask && config.randomizeFlaskLevel)
            {
                *reinterpret_cast<float*>(
                    player.playerGameData + PlayerOffsets::HpEstusRate)
                    = activeFlask.hpRate;
                *reinterpret_cast<uint8_t*>(
                    player.playerGameData + PlayerOffsets::HpEstusAdditional)
                    = activeFlask.hpAdditional;
                *reinterpret_cast<float*>(
                    player.playerGameData + PlayerOffsets::FpEstusRate)
                    = activeFlask.fpRate;
                *reinterpret_cast<uint8_t*>(
                    player.playerGameData + PlayerOffsets::FpEstusAdditional)
                    = activeFlask.fpAdditional;
            }

            // Continuous full-fight top-up. The HUD/fight reads the live
            // values from the character module, so write both that and the
            // PlayerGameData fields every poll until the fight ends.
            if (config.fillVigorOnRandomize || config.fillFpOnRandomize)
            {
                FillPlayerResources(
                    player.playerGameData,
                    player.mainPlayer,
                    config.fillVigorOnRandomize,
                    config.fillFpOnRandomize);
            }
        }

        // HP/FP top-up settle window: keep the refill applied for a few seconds
        // so the engine's own recalculation can't wipe it right after we write.
        {
            uint64_t nowTick = GetTickCount64();

            if (fillUntilTick != 0 && nowTick < fillUntilTick)
            {
                bool authoritative = player.authoritative &&
                    player.playerGameData != 0;

                uintptr_t resourcesTarget = authoritative
                    ? player.playerGameData
                    : StatBaseForGameDataMan(currentGameDataMan);

                uintptr_t chrInsTarget = authoritative
                    ? player.mainPlayer
                    : 0;

                FillPlayerResources(
                    resourcesTarget,
                    chrInsTarget,
                    config.fillVigorOnRandomize,
                    config.fillFpOnRandomize);
            }
            else if (fillUntilTick != 0)
            {
                fillUntilTick = 0;
            }
        }

        // When the boss fight ends, put the original base stats (Wondrous
        // Physick tears, flask state) back so the randomization only applies
        // during the encounter, refill HP/FP, and clear the ini cache.
        if ((haveSavedBaseStats || haveSavedPhysick || haveSavedFlask) &&
            !bossFightActive)
        {
            bool restoredStats = RestoreSavedBaseStats(
                currentGameDataMan,
                savedBaseStats,
                haveSavedBaseStats);

            bool restoredPhysick = RestoreSavedPhysick(
                currentGameDataMan,
                savedPhysick,
                haveSavedPhysick);

            bool restoredFlask = RestoreSavedFlask(
                currentGameDataMan,
                savedFlask,
                haveSavedFlask);

            if (restoredStats || restoredPhysick || restoredFlask)
            {
                std::ostringstream restoredLine;

                restoredLine << "Boss fight ended; original "
                    << (restoredStats ? "stats" : "")
                    << ((restoredStats && (restoredPhysick || restoredFlask))
                        ? ", " : "")
                    << (restoredPhysick ? "wondrous physick" : "")
                    << ((restoredPhysick && restoredFlask) ? ", " : "")
                    << (restoredFlask ? "flasks" : "")
                    << " restored.";

                Log(restoredLine.str());
            }

            if (config.cacheInitialStats)
            {
                ClearCachedStatsFromIni();
            }

            // Top resources back up after the fight (stats returned to
            // normal can leave HP/FP at randomized-fight levels) and keep the
            // refill applied for the same settling window as on apply.
            FillPlayerResources(
                StatBaseForGameDataMan(currentGameDataMan),
                player.mainPlayer,
                config.fillVigorOnRandomize,
                config.fillFpOnRandomize);

            if (config.fillVigorOnRandomize || config.fillFpOnRandomize)
            {
                constexpr uint64_t kFillSettleMilliseconds = 3000;
                fillUntilTick = GetTickCount64() + kFillSettleMilliseconds;
            }

            haveSavedBaseStats = false;
            haveSavedPhysick = false;
            haveSavedFlask = false;
            haveActivePhysick = false;
            haveActiveFlask = false;
        }

        // Startup recovery: a previous session may have crashed or exited
        // mid-boss-fight, leaving the original stats cached in the ini.
        // This retries until the restored values are confirmed settled in
        // memory. During the load-in the engine can overwrite the first write,
        // so the cache is cleared only after a successful verify.
        if (config.cacheInitialStats && !cacheRestoreDone)
        {
            uint32_t cachedStats[PlayerOffsets::AttributeCount] = { 0 };
            uint32_t cachedPhysick[PlayerOffsets::PhysickSlotCount] = { 0 };
            SavedFlaskState cachedFlask;
            bool cachedFlaskFound = false;

            bool hasCache = ReadCachedStatsFromIni(
                cachedStats, cachedPhysick, &cachedFlask, &cachedFlaskFound);

            if (!hasCache)
            {
                cacheRestoreDone = true;
            }
            else if (player.authoritative && player.playerGameData)
            {
                ++cacheRestoreAttempts;

                if (ApplyCachedRestore(
                    player.playerGameData,
                    cachedStats,
                    cachedPhysick,
                    cachedFlask,
                    cachedFlaskFound,
                    config.fillVigorOnRandomize,
                    config.fillFpOnRandomize))
                {
                    ClearCachedStatsFromIni();
                    cacheRestoreDone = true;

                    Log("Restored cached original stats from a previous "
                        "session that ended mid-boss-fight.");
                }
                else if (!cacheRestoreWarned && cacheRestoreAttempts >= 50)
                {
                    cacheRestoreWarned = true;

                    Log("Cached stats restore is still being overwritten by "
                        "the character load-in; keeping retries up.");
                }
            }
        }

        if (event.valid)
        {
            // --------------------------------------------------
            // Deterministic gate result.
            // --------------------------------------------------

            int maxByIndex[PlayerOffsets::AttributeCount] = { 0 };
            ResolveAttributeCaps(config, maxByIndex);

            uint64_t gateKey = event.gate.Key();

            GateAttributeResult computed =
                ComputeGateResult(
                    config.globalSeed,
                    event.gate,
                    config.minAttribute,
                    maxByIndex
                );

            // The per-attribute caps are part of the result, so fold them
            // into the cache identity (editing the ini mid-session must not
            // reuse a stale result).
            uint64_t capsFingerprint = 0;

            for (int i = 0; i < PlayerOffsets::AttributeCount; ++i)
            {
                capsFingerprint ^=
                    DeterministicHash::Finalize64(
                        static_cast<uint64_t>(maxByIndex[i]) *
                        (static_cast<uint64_t>(i) + 1u));
            }

            uint64_t cacheKey =
                DeterministicHash::Mix(gateKey, capsFingerprint);

            bool cached = false;

            auto cachedIt = gateResultCache.find(cacheKey);

            if (cachedIt != gateResultCache.end())
            {
                cached = (cachedIt->second == computed);
            }
            else
            {
                gateResultCache.emplace(cacheKey, computed);
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
                    config,
                    maxByIndex,
                    savedBaseStats,
                    &haveSavedBaseStats,
                    savedPhysick,
                    &haveSavedPhysick,
                    &savedFlask,
                    &haveSavedFlask,
                    activePhysick,
                    &haveActivePhysick,
                    &activeFlask,
                    &haveActiveFlask,
                    &fillUntilTick
                );

            if (applied.accepted)
            {
                std::ostringstream appliedLine;

                appliedLine << "Gate result applied successfully"
                    << " | fields written: "
                    << applied.applied
                    << " | attributes: "
                    << computed.attributes[0] << " "
                    << computed.attributes[1] << " "
                    << computed.attributes[2] << " "
                    << computed.attributes[3] << " "
                    << computed.attributes[4] << " "
                    << computed.attributes[5] << " "
                    << computed.attributes[6] << " "
                    << computed.attributes[7];

                if (applied.physickApplied > 0)
                {
                    appliedLine << " | physick tears shuffled: "
                        << applied.physickApplied;
                }

                if (applied.flaskApplied > 0)
                {
                    appliedLine << " | flask fields written: "
                        << applied.flaskApplied;
                }

                Log(appliedLine.str());
            }
            else
            {
                std::ostringstream rejectedLine;

                rejectedLine << "Gate result NOT APPLIED";

                if (!config.enableRuntimeApplication)
                {
                    rejectedLine << " (EnableRuntimeApplication is OFF "
                        "or config not loaded)";
                }
                else
                {
                    rejectedLine << " (fail-closed)"
                        << " | written: "
                        << applied.applied
                        << " | rejected: "
                        << applied.rejected;
                }

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