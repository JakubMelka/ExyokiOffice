// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "FuzzTargets.hpp"

namespace ExyokiOffice::Fuzz
{

std::span<const FuzzTargetInfo> AllTargets() noexcept
{
    static constexpr FuzzTargetInfo targets[] = {
        {"flatopc", &RunFlatOpc},
        {"packageload", &RunPackageLoad},
        {"xmlpart", &RunXmlPart},
        {"simpletypes", &RunSimpleTypes},
        {"formula", &RunFormula},
        {"copyops", &RunCopyOps},
#if defined(EXYOKIOFFICE_FUZZ_HAS_MCP)
        {"mcprpc", &RunMcpRpc},
#endif
    };

    return std::span<const FuzzTargetInfo>(targets);
}

} // namespace ExyokiOffice::Fuzz
