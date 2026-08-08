// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Version.hpp"

namespace ExyokiOffice
{

std::string_view GetVersion() noexcept
{
    return Version::String;
}

std::string_view GetAbiVersion() noexcept
{
    return Version::Abi;
}

} // namespace ExyokiOffice
