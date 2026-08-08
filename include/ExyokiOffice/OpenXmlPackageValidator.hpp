// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/OpenXmlDomValidator.hpp"
#include "ExyokiOffice/ValidationResult.hpp"

namespace ExyokiOffice
{

class OpenXmlPackage;

class EXYOKIOFFICE_EXPORT OpenXmlPackageValidator
{
public:
    /** Creates the backwards-compatible OPC and package-semantic validator. */
    OpenXmlPackageValidator() noexcept = default;

    /** Enables universal DOM/schema validation for every Word, Excel, or PowerPoint XML part. */
    explicit OpenXmlPackageValidator(OpenXmlDomValidationSettings domSettings) noexcept;

    [[nodiscard]] ValidationResult Validate(const OpenXmlPackage& package) const;
    void Validate(const OpenXmlPackage& package, DiagnosticSink& sink) const;

private:
    OpenXmlDomValidationSettings domSettings_;
    bool validateDom_{false};
};

} // namespace ExyokiOffice
