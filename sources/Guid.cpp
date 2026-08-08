// Copyright (c) 2026 Jakub Melka and Collaborators
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
    thread_local std::mt19937_64 generator = []
    {
        std::random_device device;
        std::seed_seq seed{device(), device(), device(), device()};
        return std::mt19937_64(seed);
    }();
    std::uniform_int_distribution<UInt64> distribution;

    auto high = distribution(generator);
    auto low = distribution(generator);
    high = (high & 0xffffffffffff0fffULL) | 0x0000000000004000ULL;
    low = (low & 0x3fffffffffffffffULL) | 0x8000000000000000ULL;

    return std::format("{{{:08X}-{:04X}-{:04X}-{:04X}-{:012X}}}", static_cast<UInt32>(high >> 32),
                       static_cast<UInt16>(high >> 16), static_cast<UInt16>(high),
                       static_cast<UInt16>(low >> 48), low & 0x0000ffffffffffffULL);
}

} // namespace ExyokiOffice
