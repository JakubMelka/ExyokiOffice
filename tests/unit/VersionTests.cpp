// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/Version.hpp"

#include <format>
#include <string>

using Version = ExyokiOffice::Version;

// The expectations are derived from the version fields rather than spelled out,
// so a release never has to edit this file: what is worth testing is that the
// pieces agree with each other and with the ABI policy in docs/ABI.md.
TEST_CASE("ExyokiOffice exposes matching compile-time and runtime versions [unit] [version]")
{
    const auto expectedVersion = std::format("{}.{}.{}", Version::Major, Version::Minor, Version::Patch);

    CHECK(std::string(Version::String) == expectedVersion);
    CHECK(std::string(ExyokiOffice::GetVersion()) == std::string(Version::String));
}

TEST_CASE("The ABI identity is the major and minor version [unit] [version]")
{
    // Only patch releases keep the ABI, so builds are compatible exactly when
    // MAJOR.MINOR matches; the patch level is deliberately absent.
    const auto expectedAbi = std::format("{}.{}", Version::Major, Version::Minor);

    CHECK(std::string(Version::Abi) == expectedAbi);
    CHECK(std::string(ExyokiOffice::GetAbiVersion()) == std::string(Version::Abi));
}
