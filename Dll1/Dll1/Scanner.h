#pragma once

#include <Windows.h>
#include <cstdint>

namespace Game
{
    uintptr_t FindPattern(
        const char* pattern,
        const char* mask
    );

    // Collects up to maxResults matches. Returns the total number of
    // matches found (which may exceed maxResults if truncated).
    size_t FindPatternAll(
        const char* pattern,
        const char* mask,
        uintptr_t* results,
        size_t maxResults
    );

    uintptr_t ResolveRIPRelative(uintptr_t address);
}