// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/Tools/PackageLimits.hpp"
#include "ExyokiOffice/Tools/PackageModel.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ExyokiOffice::Tools
{

struct EXYOKIOFFICE_EXPORT ToFlatOpcOptions
{
    bool PrettyPrint = true;
    /**
     * @brief ZIP safety limits applied to the package being converted.
     *
     * The conversion reads the archive itself instead of going through the OPC
     * loader, so it needs its own copy of the guard: without one, a caller
     * pointing `ConvertToFlatOpc` at an uploaded file would allocate whatever
     * uncompressed size the ZIP directory declares. Entry sizes are checked
     * before an entry is decompressed, exactly as the loader checks them.
     *
     * See Tools::DefaultPackageLimits for what the default is.
     */
    OpenXmlPackageLimits Limits = DefaultPackageLimits();
};

struct EXYOKIOFFICE_EXPORT ToFlatOpcResult
{
    bool Ok = false;
    Size PartCount = 0;
    std::string FlatOpcXml;
    std::vector<ToolDiagnostic> Diagnostics;
};

struct EXYOKIOFFICE_EXPORT FromFlatOpcOptions
{
    int CompressionLevel = 6;
};

struct EXYOKIOFFICE_EXPORT FromFlatOpcResult
{
    bool Ok = false;
    Size PartCount = 0;
    std::vector<Byte> PackageBytes;
    std::vector<ToolDiagnostic> Diagnostics;
};

EXYOKIOFFICE_EXPORT ToFlatOpcResult ConvertToFlatOpc(std::span<const Byte> packageBytes,
                                                     const ToFlatOpcOptions& options = {});
EXYOKIOFFICE_EXPORT ToFlatOpcResult ConvertToFlatOpc(const std::filesystem::path& packagePath,
                                                     const std::filesystem::path& outFile,
                                                     const ToFlatOpcOptions& options = {});
EXYOKIOFFICE_EXPORT FromFlatOpcResult ConvertFromFlatOpc(std::string_view flatOpcXml,
                                                         const FromFlatOpcOptions& options = {});
EXYOKIOFFICE_EXPORT FromFlatOpcResult ConvertFromFlatOpc(const std::filesystem::path& flatOpcPath,
                                                         const std::filesystem::path& outPackage,
                                                         const FromFlatOpcOptions& options = {});

} // namespace ExyokiOffice::Tools
