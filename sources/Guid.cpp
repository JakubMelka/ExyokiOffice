// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Guid.hpp"

#include "ExyokiOffice/StandardTypes.hpp"

#include <format>
#include <random>

namespace ExyokiOffice
{

std::string Guid::New()
{
    // Drawn straight from the operating system's entropy source. A GUID is
    // minted rarely and needs uniqueness above all, and taking the bits per
    // call keeps the shared library free of generator state - no global,
    // no thread-local storage.
    std::random_device device;
    const auto randomWord = [&device]
    {
        return (static_cast<UInt64>(device()) << 32) | static_cast<UInt64>(device());
    };

    auto high = randomWord();
    auto low = randomWord();
    high = (high & 0xffffffffffff0fffULL) | 0x0000000000004000ULL;
    low = (low & 0x3fffffffffffffffULL) | 0x8000000000000000ULL;

    return std::format("{{{:08X}-{:04X}-{:04X}-{:04X}-{:012X}}}", static_cast<UInt32>(high >> 32),
                       static_cast<UInt16>(high >> 16), static_cast<UInt16>(high),
                       static_cast<UInt16>(low >> 48), low & 0x0000ffffffffffffULL);
}

} // namespace ExyokiOffice
