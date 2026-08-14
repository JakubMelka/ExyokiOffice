// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Tools/MediaExporter.hpp"

#include "ExyokiOffice/ImageFormat.hpp"
#include "ExyokiOffice/Tools/OutputNaming.hpp"
#include "ExyokiOffice/Tools/PackageInspector.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <fstream>
#include <unordered_map>

namespace ExyokiOffice::Tools
{

/// File-local helpers behind media export.
class MediaExporterHelper
{
public:
    static bool IsMediaContentType(std::string_view contentType)
    {
        return contentType.starts_with("image/") || contentType.starts_with("audio/") ||
               contentType.starts_with("video/");
    }

    static bool IsEmbeddedObjectPart(std::string_view contentType, std::string_view uri)
    {
        return contentType.find("oleObject") != std::string_view::npos ||
               contentType.find("package") != std::string_view::npos || uri.find("/embeddings/") != std::string_view::npos;
    }

    static const std::unordered_map<std::string, std::string>& ContentTypeExtensionTable()
    {
        static const std::unordered_map<std::string, std::string> table = {
            {"image/png", "png"},
            {"image/jpeg", "jpg"},
            {"image/gif", "gif"},
            {"image/bmp", "bmp"},
            {"image/x-emf", "emf"},
            {"image/x-wmf", "wmf"},
            {"image/tiff", "tiff"},
            {"audio/mpeg", "mp3"},
            {"audio/wav", "wav"},
            {"video/mp4", "mp4"},
            {"video/x-msvideo", "avi"},
            {"application/vnd.openxmlformats-officedocument.oleObject", "bin"},
        };
        return table;
    }

    static std::string ExtensionFromUri(std::string_view uri)
    {
        const auto slash = uri.rfind('/');
        const auto fileName = slash == std::string_view::npos ? uri : uri.substr(slash + 1);
        const auto dot = fileName.rfind('.');
        if (dot == std::string_view::npos || dot + 1 >= fileName.size())
        {
            return {};
        }
        return std::string(fileName.substr(dot + 1));
    }

    static std::string BaseNameFromUri(std::string_view uri)
    {
        const auto slash = uri.rfind('/');
        const auto fileName = slash == std::string_view::npos ? uri : uri.substr(slash + 1);
        const auto dot = fileName.rfind('.');
        return std::string(dot == std::string_view::npos ? fileName : fileName.substr(0, dot));
    }

    static std::string ResolveExtension(std::string_view uri, std::string_view contentType, const std::vector<Byte>& data)
    {
        // Sniff image bytes first: the part's stored extension can lie (generic ".bin" is common),
        // so a real signature match always wins over a claimed extension for image payloads.
        if (contentType.starts_with("image/"))
        {
            if (auto format = DetectImageFormat(data))
            {
                // ImageFormatInfo::Extension includes the leading dot (e.g. ".bmp").
                auto detected = format->Extension;
                if (!detected.empty() && detected.front() == '.')
                {
                    detected.erase(detected.begin());
                }
                return detected;
            }
        }

        auto extension = ExtensionFromUri(uri);
        if (!extension.empty())
        {
            return extension;
        }

        const auto& table = ContentTypeExtensionTable();
        const auto it = table.find(std::string(contentType));
        return it != table.end() ? it->second : std::string("bin");
    }

    static std::filesystem::path MakeUniquePath(const std::filesystem::path& outDir, const std::string& baseName,
                                                const std::string& extension, std::vector<std::string>& usedNames)
    {
        auto candidateName = baseName + "." + extension;
        int suffix = 1;
        while (std::find(usedNames.begin(), usedNames.end(), candidateName) != usedNames.end())
        {
            ++suffix;
            candidateName = baseName + "-" + std::to_string(suffix) + "." + extension;
        }
        usedNames.push_back(candidateName);
        return outDir / candidateName;
    }
};

MediaExportResult ExportMedia(const OpenXmlPackage& package, const std::filesystem::path& outDir, bool overwrite)
{
    MediaExportResult result;

    std::filesystem::create_directories(outDir);

    std::vector<std::string> usedNames;
    for (const auto& part : CollectAllParts(package))
    {
        const auto contentType = std::string(part->ContentType());
        const auto& uri = part->Uri();
        if (!part->IsBinaryPart() || !(MediaExporterHelper::IsMediaContentType(contentType) || MediaExporterHelper::IsEmbeddedObjectPart(contentType, uri)))
        {
            continue;
        }

        const auto data = part->GetBinaryData();

        // Both halves of the file name come out of the package, which is not
        // trusted input: a part called `/word/media/NUL.png` would write to the
        // console device and report success, and `image1.png:hidden` would put
        // the payload in an alternate data stream of a file the caller sees as
        // ordinary. Part URIs are normalized on load, so a separator cannot get
        // this far, but the remaining Windows spellings still can.
        const auto rawExtension = MediaExporterHelper::ResolveExtension(uri, contentType, data);
        const auto rawBaseName = MediaExporterHelper::BaseNameFromUri(uri);
        const auto extension = MakePlainOutputName(rawExtension, "bin");
        const auto baseName = MakePlainOutputName(rawBaseName, "media");
        if (baseName != rawBaseName || extension != rawExtension)
        {
            result.Diagnostics.push_back(
                ToolDiagnostic{ToolSeverity::Warning,
                               "Part name is not usable as a file name and was replaced by '" + baseName + "." +
                                   extension + "'",
                               uri});
        }

        const auto outputPath = MediaExporterHelper::MakeUniquePath(outDir, baseName, extension, usedNames);

        // The name is a single plain component by construction, so this holds
        // today; it is checked because containment is the property the whole
        // function has to keep, and the next change to how a name is derived
        // would otherwise break it silently.
        if (!IsInsideDirectory(outDir, outputPath))
        {
            result.Diagnostics.push_back(
                ToolDiagnostic{ToolSeverity::Error, "Output file would leave the output directory", outputPath.string()});
            continue;
        }

        if (std::filesystem::exists(outputPath) && !overwrite)
        {
            result.Diagnostics.push_back(
                ToolDiagnostic{ToolSeverity::Error, "Output file already exists", outputPath.string()});
            continue;
        }

        std::ofstream file(outputPath, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            result.Diagnostics.push_back(
                ToolDiagnostic{ToolSeverity::Error, "Failed to write media file", outputPath.string()});
            continue;
        }
        file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));

        MediaExportItem item;
        item.PartUri = uri;
        item.OutputPath = outputPath;
        item.ContentType = contentType;
        item.Size = data.size();
        result.Items.push_back(std::move(item));
    }

    result.Ok = true;
    return result;
}

} // namespace ExyokiOffice::Tools
