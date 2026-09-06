#include "pch.h"
#include "Scanner.h"

namespace Game
{
    uintptr_t FindPattern(
        const char* pattern,
        const char* mask)
    {
        HMODULE module = GetModuleHandleA(nullptr);

        if (!module)
            return 0;

        auto dosHeader =
            reinterpret_cast<PIMAGE_DOS_HEADER>(module);

        if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
            return 0;

        auto ntHeaders =
            reinterpret_cast<PIMAGE_NT_HEADERS>(
                reinterpret_cast<uintptr_t>(module) +
                dosHeader->e_lfanew
                );

        if (ntHeaders->Signature != IMAGE_NT_SIGNATURE)
            return 0;

        uintptr_t base =
            reinterpret_cast<uintptr_t>(module);

        size_t imageSize =
            ntHeaders->OptionalHeader.SizeOfImage;

        size_t patternLength = strlen(mask);

        if (patternLength == 0 ||
            patternLength > imageSize)
        {
            return 0;
        }

        for (size_t i = 0;
            i <= imageSize - patternLength;
            ++i)
        {
            bool found = true;

            for (size_t j = 0;
                j < patternLength;
                ++j)
            {
                if (mask[j] != '?' &&
                    pattern[j] !=
                    *reinterpret_cast<const char*>(
                        base + i + j))
                {
                    found = false;
                    break;
                }
            }

            if (found)
                return base + i;
        }

        return 0;
    }

    size_t FindPatternAll(
        const char* pattern,
        const char* mask,
        uintptr_t* results,
        size_t maxResults)
    {
        HMODULE module = GetModuleHandleA(nullptr);

        if (!module)
            return 0;

        auto dosHeader =
            reinterpret_cast<PIMAGE_DOS_HEADER>(module);

        if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
            return 0;

        auto ntHeaders =
            reinterpret_cast<PIMAGE_NT_HEADERS>(
                reinterpret_cast<uintptr_t>(module) +
                dosHeader->e_lfanew
                );

        if (ntHeaders->Signature != IMAGE_NT_SIGNATURE)
            return 0;

        uintptr_t base =
            reinterpret_cast<uintptr_t>(module);

        size_t imageSize =
            ntHeaders->OptionalHeader.SizeOfImage;

        size_t patternLength = strlen(mask);

        if (patternLength == 0 ||
            patternLength > imageSize)
        {
            return 0;
        }

        size_t count = 0;

        for (size_t i = 0;
            i <= imageSize - patternLength;
            ++i)
        {
            bool found = true;

            for (size_t j = 0;
                j < patternLength;
                ++j)
            {
                if (mask[j] != '?' &&
                    pattern[j] !=
                    *reinterpret_cast<const char*>(
                        base + i + j))
                {
                    found = false;
                    break;
                }
            }

            if (found)
            {
                ++count;

                if (results && count <= maxResults)
                {
                    results[count - 1] = base + i;
                }
            }
        }

        return count;
    }
}