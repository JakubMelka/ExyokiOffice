// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/Version.hpp"

#include <string>

// The expectations are derived from the version fields rather than spelled out,
// so a release never has to edit this file: what is worth testing is that the
// pieces agree with each other and with the ABI policy in docs/ABI.md.
TEST_CASE("ExyokiOffice exposes matching compile-time and runtime versions [unit] [version]")
{
    const std::string expectedVersion = std::to_string(ExyokiOffice::Version::Major) + "."
                                        + std::to_string(ExyokiOffice::Version::Minor) + "."
                                        + std::to_string(ExyokiOffice::Version::Patch);

    CHECK(std::string(ExyokiOffice::Version::String) == expectedVersion);
    CHECK(std::string(ExyokiOffice::GetVersion()) == std::string(ExyokiOffice::Version::String));
}

TEST_CASE("The ABI identity is the major and minor version [unit] [version]")
{
    // Only patch releases keep the ABI, so builds are compatible exactly when
    // MAJOR.MINOR matches; the patch level is deliberately absent.
    const std::string expectedAbi = std::to_string(ExyokiOffice::Version::Major) + "."
                                    + std::to_string(ExyokiOffice::Version::Minor);

    CHECK(std::string(ExyokiOffice::Version::Abi) == expectedAbi);
    CHECK(std::string(ExyokiOffice::GetAbiVersion()) == std::string(ExyokiOffice::Version::Abi));
}
