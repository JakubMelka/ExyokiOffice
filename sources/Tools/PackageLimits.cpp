// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Tools/PackageLimits.hpp"

namespace ExyokiOffice::Tools
{

OpenXmlPackageLimits DefaultPackageLimits()
{
    // ConfiguredDefaultPackageLimits, not DefaultPackageLimits: only the former
    // distinguishes a policy the application installed - including a deliberate
    // Unlimited() - from the safe default substituted for a choice nobody made.
    if (auto configured = OpenXmlPackage::ConfiguredDefaultPackageLimits())
    {
        return *configured;
    }

    return OpenXmlPackageLimits::Recommended();
}

void ApplyDefaultPackageLimits(OpenXmlPackage& package)
{
    package.SetPackageLimits(DefaultPackageLimits());
}

Packaging::OpenSettings UntrustedOpenSettings()
{
    Packaging::OpenSettings settings;
    settings.PackageLimits = DefaultPackageLimits();
    return settings;
}

Packaging::OpenSettings OwnOutputOpenSettings()
{
    Packaging::OpenSettings settings;
    settings.PackageLimits = OpenXmlPackageLimits::Unlimited();
    return settings;
}

} // namespace ExyokiOffice::Tools
