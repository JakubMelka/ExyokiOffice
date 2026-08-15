// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/Tools/PackageModel.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace ExyokiOffice::Tools
{

/// One exported media/embedded-object file.
struct EXYOKIOFFICE_EXPORT MediaExportItem
{
    /// Source part URI inside the package.
    std::string PartUri;
    /// File written under the output directory.
    std::filesystem::path OutputPath;
    std::string ContentType;
    UInt64 Size = 0;
};

/// Result of ExportMedia().
struct EXYOKIOFFICE_EXPORT MediaExportResult
{
    bool Ok = false;
    std::vector<MediaExportItem> Items;
    std::vector<ToolDiagnostic> Diagnostics;
};

/**
 * @brief Exports every image/audio/video/embedded-object part to a flat directory.
 *
 * Selects binary parts whose content type starts with "image/", "audio/", or
 * "video/", plus OLE/embedded-package parts (content type containing
 * "oleObject" or "package", or a part URI under an "/embeddings/" folder).
 * The output file name is derived from the part's URI file name; when that
 * name is missing a recognizable extension, DetectImageFormat is used
 * to recover one for image payloads (falling back to the part's content
 * type). Name collisions in the output directory are resolved with a
 * numeric suffix (`image1.png`, `image1-2.png`, ...).
 */
EXYOKIOFFICE_EXPORT MediaExportResult ExportMedia(const OpenXmlPackage& package,
                                                  const std::filesystem::path& outDir,
                                                  bool overwrite = false);

} // namespace ExyokiOffice::Tools
