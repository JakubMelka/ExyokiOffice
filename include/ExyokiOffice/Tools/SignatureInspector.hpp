// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/Tools/PackageLimits.hpp"
#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/Security/PackageSignatures.hpp"

#include <filesystem>

namespace ExyokiOffice::Tools
{

/// Options controlling a single InspectSignatures() invocation.
struct EXYOKIOFFICE_EXPORT SignatureInspectionOptions
{
    /// ZIP/XML safety limits applied before loading; see Tools::DefaultPackageLimits.
    OpenXmlPackageLimits Limits = DefaultPackageLimits();
};

/// What a package says about its digital signatures, and how much of it holds.
struct EXYOKIOFFICE_EXPORT SignatureInspectionReport
{
    /// False when the package failed to load at all.
    bool Loaded = false;
    /// Verification outcome; empty when the package carries no signatures.
    Security::VerifySignaturesResult Result;
};

/**
 * @brief Loads a package and reports on the digital signatures it carries.
 *
 * No crypto provider is involved, so this answers whether the signed content is
 * unchanged, not who signed it. Checking the signature value needs a provider
 * and therefore the library API, ExyokiOffice::Security::VerifySignatures.
 */
EXYOKIOFFICE_EXPORT SignatureInspectionReport InspectSignatures(const std::filesystem::path& path,
                                                                const SignatureInspectionOptions& options = {});

/// Same as InspectSignatures(path, options) but inspects an already-loaded package.
EXYOKIOFFICE_EXPORT SignatureInspectionReport InspectSignatures(const OpenXmlPackage& package);

} // namespace ExyokiOffice::Tools
