// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Tools/SignatureInspector.hpp"

namespace ExyokiOffice::Tools
{

SignatureInspectionReport InspectSignatures(const OpenXmlPackage& package)
{
    SignatureInspectionReport report;
    report.Loaded = true;
    report.Result = Security::VerifySignatures(package);
    return report;
}

SignatureInspectionReport InspectSignatures(const std::filesystem::path& path,
                                            const SignatureInspectionOptions& options)
{
    OpenXmlPackage package;
    package.SetPackageLimits(options.Limits);
    // Verifying a signature needs the bytes the parts were stored with, which
    // the default retention policy keeps for packages that carry signatures.
    if (!package.LoadFromFile(path))
    {
        return {};
    }
    return InspectSignatures(package);
}

} // namespace ExyokiOffice::Tools
