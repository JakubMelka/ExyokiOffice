// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/Tools/PackageLimits.hpp"
#include "ExyokiOffice/FileFormatVersions.h"
#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/ValidationResult.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace ExyokiOffice::Tools
{

/// Options controlling a single ValidationRunner::Run() invocation.
struct EXYOKIOFFICE_EXPORT ValidationRunOptions
{
    /// Office generation the DOM validator checks schema availability against.
    ExyokiOffice::OpenXml::FileFormatVersions TargetVersion = ExyokiOffice::OpenXml::FileFormatVersions::Microsoft365;
    /// When false, only OPC/package-level validation runs (no per-part DOM/schema checks).
    bool RunDomValidation = true;
    /**
     * @brief Checks every content-model verdict against the reference matcher.
     *
     * Passed straight to OpenXmlDomValidationSettings::CrossCheckContentModel:
     * both matchers run on every element and a
     * ValidationErrorId::ContentModelCrossCheckMismatch is reported wherever
     * they disagree, which is a defect in this library rather than in the
     * document. Off by default, and much slower when on.
     */
    bool CrossCheckContentModel = false;
    /// ZIP/XML safety limits applied before loading; see Tools::DefaultPackageLimits.
    OpenXmlPackageLimits Limits = DefaultPackageLimits();
};

/// Aggregate result of validating one package, combining load and validator diagnostics.
struct EXYOKIOFFICE_EXPORT ValidationReport
{
    /// False when the package failed to load at all (issues are in LoadIssues).
    bool Loaded = false;
    /// Diagnostics captured while loading the package (OPC-level, if OpcValidation was enabled).
    std::vector<ValidationIssue> LoadIssues;
    /// Diagnostics from OpenXmlPackageValidator (OPC + optional DOM/schema validation).
    std::vector<ValidationIssue> ValidationIssues;
    Size ErrorCount = 0;
    Size WarningCount = 0;
};

/**
 * @brief Loads a package from disk and runs OPC + optional DOM/schema validation.
 *
 * This is a thin wrapper: SetPackageLimits, LoadFromFile (capturing OPC load
 * diagnostics from LastValidationResult()), then OpenXmlPackageValidator
 * (constructed with OpenXmlDomValidationSettings when RunDomValidation is
 * true, or default-constructed otherwise) validating the loaded package.
 * ErrorCount/WarningCount count both LoadIssues and ValidationIssues.
 */
EXYOKIOFFICE_EXPORT ValidationReport Run(const std::filesystem::path& path, const ValidationRunOptions& options = {});

/// Same as Run(path, options) but validates an already-loaded package.
EXYOKIOFFICE_EXPORT ValidationReport Run(OpenXmlPackage& package, const ValidationRunOptions& options = {});

} // namespace ExyokiOffice::Tools
