// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/StandardTypes.hpp"

#include <cstdint>

namespace ExyokiOffice::OpenXml
{
enum class FileFormatVersions : UInt32
{
    None = 0x0,
    Office2007 = 0x1,
    Office2010 = 0x2,
    Office2013 = 0x4,
    Office2016 = 0x8,
    Office2019 = 0x10,
    Office2021 = 0x20,
    Microsoft365 = 0x40000000,
};

constexpr FileFormatVersions operator|(FileFormatVersions lhs, FileFormatVersions rhs) noexcept
{
    return static_cast<FileFormatVersions>(
        static_cast<UInt32>(lhs) | static_cast<UInt32>(rhs));
}

constexpr FileFormatVersions operator&(FileFormatVersions lhs, FileFormatVersions rhs) noexcept
{
    return static_cast<FileFormatVersions>(
        static_cast<UInt32>(lhs) & static_cast<UInt32>(rhs));
}

constexpr FileFormatVersions operator^(FileFormatVersions lhs, FileFormatVersions rhs) noexcept
{
    return static_cast<FileFormatVersions>(
        static_cast<UInt32>(lhs) ^ static_cast<UInt32>(rhs));
}

constexpr FileFormatVersions operator~(FileFormatVersions value) noexcept
{
    return static_cast<FileFormatVersions>(~static_cast<UInt32>(value));
}

inline FileFormatVersions& operator|=(FileFormatVersions& lhs, FileFormatVersions rhs) noexcept
{
    lhs = lhs | rhs;
    return lhs;
}

inline FileFormatVersions& operator&=(FileFormatVersions& lhs, FileFormatVersions rhs) noexcept
{
    lhs = lhs & rhs;
    return lhs;
}

inline FileFormatVersions& operator^=(FileFormatVersions& lhs, FileFormatVersions rhs) noexcept
{
    lhs = lhs ^ rhs;
    return lhs;
}

} // namespace ExyokiOffice::OpenXml
