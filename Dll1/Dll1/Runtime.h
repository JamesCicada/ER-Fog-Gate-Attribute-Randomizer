#pragma once

#include <Windows.h>
#include <cstdint>

namespace Game
{
    struct ModuleInfo
    {
        uintptr_t baseAddress;
        size_t imageSize;
    };

    inline ModuleInfo GetMainModule()
    {
        HMODULE module = GetModuleHandleA(nullptr);

        if (!module)
            return { 0, 0 };

        auto dosHeader =
            reinterpret_cast<PIMAGE_DOS_HEADER>(module);

        auto ntHeaders =
            reinterpret_cast<PIMAGE_NT_HEADERS>(
                reinterpret_cast<uintptr_t>(module) +
                dosHeader->e_lfanew
                );

        return {
            reinterpret_cast<uintptr_t>(module),
            ntHeaders->OptionalHeader.SizeOfImage
        };
    }

    inline uintptr_t GetGameDataManInstance(
        uintptr_t globalPointerAddress)
    {
        if (!globalPointerAddress)
            return 0;

        return *reinterpret_cast<uintptr_t*>(
            globalPointerAddress
            );
    }
    inline uintptr_t ResolveRIPRelative(uintptr_t instruction)
    {
        if (!instruction)
            return 0;

        int32_t displacement =
            *reinterpret_cast<int32_t*>(instruction + 3);

        return instruction + displacement + 7;
    }

    inline uintptr_t GetWorldChrManInstance(uintptr_t globalPointerAddress)
    {
        if (!globalPointerAddress)
            return 0;

        return *reinterpret_cast<uintptr_t*>(
            globalPointerAddress
            );
    }
}