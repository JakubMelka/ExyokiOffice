// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "FuzzTargets.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

extern "C" int LLVMFuzzerTestOneInput(const ExyokiOffice::UInt8* data, ExyokiOffice::Size size)
{
    return ExyokiOffice::Fuzz::RunCopyOps(data, size);
}
