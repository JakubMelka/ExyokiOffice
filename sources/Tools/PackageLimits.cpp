// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Tools/PackageLimits.hpp"

namespace ExyokiOffice::Tools
{

OpenXmlPackageLimits DefaultPackageLimits()
{
    // ConfiguredDefaultPackageLimits, not DefaultPackageLimits: the latter
    // reports Unlimited() both when the application chose it and when it never
    // chose anything, and those two have to lead to different answers here.
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
