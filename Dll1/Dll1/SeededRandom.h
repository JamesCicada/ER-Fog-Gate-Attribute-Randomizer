#pragma once

#include <cstdint>

#include "PlayerOffsets.h"

// ============================================================
// Deterministic seeding and randomization.
//
// The randomization for a fog gate must be a pure function of:
//
//     gate result = f(global_seed, stable_gate_id)
//
// It must NOT depend on runtime RNG state, pointer addresses,
// visit counts, wall-clock time, or discovery order.
//
// All operations below use only unsigned integer arithmetic, so
// wrap-around and results are identical on every platform and
// compiler. No std::random_device / time / addresses are used.
// ============================================================

namespace DeterministicHash
{
    // MurmurHash3 64-bit finalizer (splitmix64-style avalanche).
    // Well-known, documented mixing function. Machine-independent.
    inline uint64_t Finalize64(uint64_t x) noexcept
    {
        x ^= x >> 33;
        x *= 0xFF51AFD7ED558CCDULL;
        x ^= x >> 33;
        x *= 0xC4CEB9FE1A85EC53ULL;
        x ^= x >> 33;
        return x;
    }

    // Mixes up to 4 independent 64-bit quantities into one 64-bit
    // value. Field order is canonical (callers pass blocks in a fixed
    // order) so the same combination always hashes identically.
    inline uint64_t Mix(
        uint64_t a,
        uint64_t b = 0,
        uint64_t c = 0,
        uint64_t d = 0) noexcept
    {
        uint64_t x = 0x9E3779B97F4A7C15ULL; // golden ratio constant
        x = Finalize64(x ^ a);
        x = Finalize64(x ^ b);
        x = Finalize64(x ^ c);
        x = Finalize64(x ^ d);
        return x;
    }
}

// Deterministic pseudo-random stream (xoshiro-style splitmix64).
// The same seed always produces the same sequence everywhere.
class SeededRandom
{
public:
    explicit SeededRandom(uint64_t seed) noexcept
    {
        state_ = seed + 0x9E3779B97F4A7C15ULL;
    }

    uint64_t NextU64() noexcept
    {
        // splitmix64
        uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    // Uniform integer in [minInclusive, maxInclusive] (inclusive).
    int NextBounded(int minInclusive, int maxInclusive) noexcept
    {
        if (maxInclusive < minInclusive)
            return minInclusive;

        uint64_t range =
            static_cast<uint64_t>(maxInclusive - minInclusive) + 1u;

        return static_cast<int>(NextU64() % range) + minInclusive;
    }

private:
    uint64_t state_;
};

// ============================================================
// Stable fog gate identity (runtime-available components).
//
// Identity priority (per spec section 10):
//   1. Native/stable gate entity id (fog wall asset entity id,
//      e.g. event 9005800 arg X4_4) - preferred, currently not yet
//      runtime-readable without an event hook (NOT VERIFIED hook).
//   2. Map/block + play region identity.
//   3. Quantized deterministic coordinates as a tiebreaker.
//
// The identity is therefore a deterministic mix of whatever parts
// are known, so that adding the entity id later automatically
// refines (changes) the identity without changing the mechanism.
// ============================================================

struct StableGateIdentity
{
    uint32_t fogWallEntityId = 0; // 9005800 X4_4 (0 when unknown)
    uint32_t blockId = 0;         // PlayerIns.current_block_id
    uint32_t playRegionId = 0;    // PlayerIns.play_region_id
    uint32_t bossNpcParamId = 0;  // GameDataMan.boss_health_bar_npc_param_id
    int32_t quantizedX = 0;       // floor(position.x / 256)
    int32_t quantizedY = 0;
    int32_t quantizedZ = 0;

    // Deterministic 64-bit key for this gate.
    uint64_t Key() const noexcept
    {
        return DeterministicHash::Mix(
            DeterministicHash::Finalize64(
                (static_cast<uint64_t>(static_cast<uint32_t>(quantizedX)) << 32) |
                 static_cast<uint64_t>(static_cast<uint32_t>(quantizedY))),
            DeterministicHash::Finalize64(
                (static_cast<uint64_t>(fogWallEntityId) << 32) |
                 static_cast<uint64_t>(playRegionId)),
            DeterministicHash::Finalize64(
                (static_cast<uint64_t>(blockId) << 32) |
                 static_cast<uint64_t>(bossNpcParamId)),
            DeterministicHash::Finalize64(
                static_cast<uint64_t>(static_cast<uint32_t>(quantizedZ))));
    }
};

// Deterministic gate seed: gateSeed = mix(globalSeed, gateId.Key()).
inline uint64_t ComputeGateSeed(
    uint64_t globalSeed,
    const StableGateIdentity& gate) noexcept
{
    return DeterministicHash::Mix(globalSeed, gate.Key());
}

// Per-gate randomized attribute result (the "gate attributes").
struct GateAttributeResult
{
    int attributes[PlayerOffsets::AttributeCount] = { 0 };
    uint64_t gateSeed = 0;
    uint64_t gateKey = 0;

    bool operator==(const GateAttributeResult& other) const noexcept
    {
        for (int i = 0; i < PlayerOffsets::AttributeCount; ++i)
        {
            if (attributes[i] != other.attributes[i])
                return false;
        }
        return
            gateSeed == other.gateSeed &&
            gateKey == other.gateKey;
    }

    bool operator!=(const GateAttributeResult& other) const noexcept
    {
        return !(*this == other);
    }
};

// Generates the deterministic attribute set for a gate.
// Same (globalSeed, gateId) -> identical result on every launch.
// maxAttributeByIndex[i] is the effective per-attribute cap for index i.
inline GateAttributeResult ComputeGateResult(
    uint64_t globalSeed,
    const StableGateIdentity& gate,
    int minAttribute,
    const int* maxAttributeByIndex) noexcept
{
    GateAttributeResult result;

    result.gateSeed = ComputeGateSeed(globalSeed, gate);
    result.gateKey = gate.Key();

    SeededRandom rng(result.gateSeed);

    for (int i = 0; i < PlayerOffsets::AttributeCount; ++i)
    {
        result.attributes[i] =
            rng.NextBounded(minAttribute, maxAttributeByIndex[i]);
    }

    return result;
}