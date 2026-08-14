// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Tools/ValidationRunner.hpp"

#include "ExyokiOffice/OpenXmlDomValidator.hpp"
#include "ExyokiOffice/OpenXmlPackageValidator.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

namespace ExyokiOffice::Tools
{

/// File-local helpers behind the validation runner.
class ValidationRunnerHelper
{
public:
    static void CountIssues(const std::vector<ValidationIssue>& issues, Size& errors, Size& warnings)
    {
        for (const auto& issue : issues)
        {
            if (issue.Severity == ValidationSeverity::Error)
            {
                ++errors;
            }
            else
            {
                ++warnings;
            }
        }
    }
};

ValidationReport Run(OpenXmlPackage& package, const ValidationRunOptions& options)
{
    ValidationReport report;
    report.Loaded = true;
    report.LoadIssues = package.LastValidationResult().Issues();

    OpenXmlDomValidationSettings settings{options.TargetVersion};
    settings.CrossCheckContentModel = options.CrossCheckContentModel;
    OpenXmlPackageValidator validator =
        options.RunDomValidation ? OpenXmlPackageValidator(settings) : OpenXmlPackageValidator();
    const auto result = validator.Validate(package);
    report.ValidationIssues = result.Issues();

    ValidationRunnerHelper::CountIssues(report.LoadIssues, report.ErrorCount, report.WarningCount);
    ValidationRunnerHelper::CountIssues(report.ValidationIssues, report.ErrorCount, report.WarningCount);
    return report;
}

ValidationReport Run(const std::filesystem::path& path, const ValidationRunOptions& options)
{
    OpenXmlPackage package;
    package.SetPackageLimits(options.Limits);
    if (!package.LoadFromFile(path))
    {
        ValidationReport report;
        report.Loaded = false;
        report.LoadIssues = package.LastValidationResult().Issues();
        ValidationRunnerHelper::CountIssues(report.LoadIssues, report.ErrorCount, report.WarningCount);
        return report;
    }

    return Run(package, options);
}

} // namespace ExyokiOffice::Tools
