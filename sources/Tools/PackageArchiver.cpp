// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Tools/PackageArchiver.hpp"

#include "pugixml/pugixml.hpp"
#include "zip/zip.h"
#include "XmlParseOptions.hpp"
#include "ExyokiOffice/StandardTypes.hpp"
#include "AsciiText.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>

namespace ExyokiOffice::Tools
{

/// File-local diagnostics and path helpers shared by Pack and Unpack.
class PackageArchiverHelper
{
public:
    static void AddDiagnostic(std::vector<ToolDiagnostic>& diagnostics, ToolSeverity severity, std::string message,
                              std::string context = {})
    {
        diagnostics.push_back(ToolDiagnostic{severity, std::move(message), std::move(context)});
    }

    static bool IsReservedWindowsName(std::string_view baseName)
    {
        static constexpr std::array<std::string_view, 22> kReserved = {
            "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
            "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};

        const std::string upper = AsciiText::ToUpper(baseName);
        return std::find(kReserved.begin(), kReserved.end(), upper) != kReserved.end();
    }

    static std::string PercentEncodeByte(unsigned char value)
    {
        static constexpr char hex[] = "0123456789ABCDEF";
        std::string result = "%";
        result.push_back(hex[value >> 4]);
        result.push_back(hex[value & 0x0F]);
        return result;
    }

    /**
     * @brief True for a character that may not appear in one path component.
     *
     * One list, consulted by both the predicate that rejects a segment and the
     * escaper that repairs it. They disagreed before — the predicate rejected a
     * segment containing a separator while the escaper left the separator in
     * place, so an escaped `a\b` still became two path components on Windows.
     * Nothing exploitable followed from it, because `..` is rejected outright, but
     * a rule stated in two places is a rule that only holds until one of them is
     * edited.
     */
    static bool IsForbiddenSegmentByte(unsigned char ch)
    {
        return ch < 0x20 || ch == '<' || ch == '>' || ch == ':' || ch == '"' || ch == '|' || ch == '?' || ch == '*' ||
               ch == '\\' || ch == '/';
    }

    /// True when @p segment is already a safe Windows path component as-is.
    static bool IsSafeWindowsSegment(std::string_view segment)
    {
        if (segment.empty() || segment == "." || segment == "..")
        {
            return false;
        }
        if (segment.back() == '.' || segment.back() == ' ')
        {
            return false;
        }

        const auto dot = segment.find('.');
        const auto baseName = segment.substr(0, dot == std::string_view::npos ? segment.size() : dot);
        if (IsReservedWindowsName(baseName))
        {
            return false;
        }

        for (const char rawCh : segment)
        {
            if (IsForbiddenSegmentByte(static_cast<unsigned char>(rawCh)))
            {
                return false;
            }
        }
        return true;
    }

    /// Percent-escapes a single unsafe path segment so it becomes a valid Windows filename.
    static std::string EscapeWindowsSegment(std::string_view segment)
    {
        std::string escaped;
        escaped.reserve(segment.size());
        for (const char rawCh : segment)
        {
            const auto ch = static_cast<unsigned char>(rawCh);
            if (IsForbiddenSegmentByte(ch))
            {
                escaped += PercentEncodeByte(ch);
            }
            else
            {
                escaped.push_back(static_cast<char>(ch));
            }
        }

        const auto dot = escaped.find('.');
        const auto baseName = escaped.substr(0, dot == std::string::npos ? escaped.size() : dot);
        if (IsReservedWindowsName(baseName) || escaped.empty())
        {
            escaped.insert(escaped.begin(), '_');
        }
        if (!escaped.empty() && (escaped.back() == '.' || escaped.back() == ' '))
        {
            // Win32 strips a trailing dot or space before opening the file, so the
            // name that is checked and the name that is created would differ. One
            // appended character is enough to make the stripping a no-op.
            escaped.push_back('_');
        }
        return escaped;
    }

    /**
     * @brief True when any component of a split entry name navigates upwards.
     *
     * Tested per component rather than with a substring search for "..": the
     * substring also matches `my..file.xml` and `..leading.png`, which are ordinary
     * names no extractor has a reason to refuse, while the component test refuses
     * exactly the navigation. Both stop the traversal; only one of them stops
     * nothing else.
     */
    static bool HasTraversalSegment(const std::vector<std::string>& segments)
    {
        return std::find(segments.begin(), segments.end(), "..") != segments.end();
    }

    static std::vector<std::string> SplitEntryPath(std::string_view entryName)
    {
        std::vector<std::string> segments;
        Size start = 0;
        while (start <= entryName.size())
        {
            const auto slash = entryName.find('/', start);
            const auto end = slash == std::string_view::npos ? entryName.size() : slash;
            if (end > start)
            {
                segments.emplace_back(entryName.substr(start, end - start));
            }
            if (slash == std::string_view::npos)
            {
                break;
            }
            start = slash + 1;
        }
        return segments;
    }

    /**
     * @brief Computes the on-disk relative path for a ZIP entry, escaping segments as needed.
     * @return {diskPath, wasEscaped}.
     */
    static std::pair<std::filesystem::path, bool> MapEntryToDiskPath(std::string_view entryName)
    {
        const auto segments = SplitEntryPath(entryName);
        std::filesystem::path diskPath;
        bool escaped = false;
        for (const auto& segment : segments)
        {
            if (IsSafeWindowsSegment(segment))
            {
                diskPath /= segment;
            }
            else
            {
                diskPath /= EscapeWindowsSegment(segment);
                escaped = true;
            }
        }
        return {diskPath, escaped};
    }

    static bool LooksLikeXml(std::string_view entryName, const std::vector<Byte>& bytes)
    {
        const bool extensionMatches = entryName.size() >= 4 &&
                                      (entryName.substr(entryName.size() - 4) == ".xml" || entryName.substr(entryName.size() - 5) == ".rels");
        if (!extensionMatches)
        {
            return false;
        }
        return !bytes.empty();
    }

    static std::vector<Byte> PrettyPrintXmlBytes(const std::vector<Byte>& bytes, bool& ok)
    {
        Pugi::xml_document doc;
        const auto result = doc.load_buffer(bytes.data(), bytes.size(), Xml::ParseOptions::Preserving);
        if (!result)
        {
            ok = false;
            return bytes;
        }
        std::ostringstream output;
        doc.save(output, "  ", Pugi::format_indent);
        const auto text = output.str();
        ok = true;
        return std::vector<Byte>(text.begin(), text.end());
    }

    static std::string XmlEscape(std::string_view text)
    {
        std::string escaped;
        escaped.reserve(text.size());
        for (char ch : text)
        {
            switch (ch)
            {
                case '&':
                    escaped.append("&amp;");
                    break;
                case '<':
                    escaped.append("&lt;");
                    break;
                case '>':
                    escaped.append("&gt;");
                    break;
                case '"':
                    escaped.append("&quot;");
                    break;
                default:
                    escaped.push_back(ch);
                    break;
            }
        }
        return escaped;
    }

    static std::string PathToPortableString(const std::filesystem::path& path)
    {
        auto generic = path.generic_string();
        return generic;
    }

    /// Built-in fallback extension -> content-type table used when regenerating `[Content_Types].xml`.
    static const std::vector<std::pair<std::string, std::string>>& DefaultContentTypeTable()
    {
        static const std::vector<std::pair<std::string, std::string>> table = {
            {"rels", "application/vnd.openxmlformats-package.relationships+xml"},
            {"xml", "application/xml"},
            {"png", "image/png"},
            {"jpeg", "image/jpeg"},
            {"jpg", "image/jpeg"},
            {"gif", "image/gif"},
            {"bmp", "image/bmp"},
            {"emf", "image/x-emf"},
            {"wmf", "image/x-wmf"},
            {"tiff", "image/tiff"},
        };
        return table;
    }
};

UnpackResult Unpack(const std::filesystem::path& packagePath, const std::filesystem::path& outDir,
                    const UnpackOptions& options)
{
    UnpackResult result;

    if (std::filesystem::exists(outDir) && !std::filesystem::is_empty(outDir) && !options.Overwrite)
    {
        PackageArchiverHelper::AddDiagnostic(result.Diagnostics, ToolSeverity::Error, "Output directory is not empty",
                                             outDir.string());
        return result;
    }

    int error = 0;
    auto* archive = zip_openwitherror(packagePath.string().c_str(), 0, 'r', &error);
    if (!archive)
    {
        PackageArchiverHelper::AddDiagnostic(result.Diagnostics, ToolSeverity::Error,
                                             std::string("Failed to open package: ") + zip_strerror(error), packagePath.string());
        return result;
    }

    std::filesystem::create_directories(outDir);

    struct RenameEntry
    {
        std::string OriginalName;
        std::string DiskRelativePath;
    };
    std::vector<RenameEntry> renames;
    bool anyPrettyPrinted = false;

    const auto total = zip_entries_total(archive);
    if (total < 0)
    {
        PackageArchiverHelper::AddDiagnostic(result.Diagnostics, ToolSeverity::Error, "ZIP entry table cannot be read",
                                             packagePath.string());
        zip_close(archive);
        return result;
    }

    if (const auto reason = options.Limits.CheckEntryCount(static_cast<UInt64>(total)); !reason.empty())
    {
        PackageArchiverHelper::AddDiagnostic(result.Diagnostics, ToolSeverity::Error, std::string(reason), packagePath.string());
        zip_close(archive);
        return result;
    }

    UInt64 compressedTotal = 0;
    UInt64 uncompressedTotal = 0;
    for (ssize_t index = 0; index < total; ++index)
    {
        if (zip_entry_openbyindex(archive, static_cast<Size>(index)) != 0)
        {
            continue;
        }

        const std::string entryName = zip_entry_name(archive) ? zip_entry_name(archive) : "";
        const bool isDir = zip_entry_isdir(archive) != 0;
        if (isDir || entryName.empty())
        {
            zip_entry_close(archive);
            continue;
        }

        const auto segments = PackageArchiverHelper::SplitEntryPath(entryName);
        if (PackageArchiverHelper::HasTraversalSegment(segments))
        {
            PackageArchiverHelper::AddDiagnostic(result.Diagnostics, ToolSeverity::Error, "Rejected entry with path traversal", entryName);
            zip_entry_close(archive);
            continue;
        }

        // Read from the ZIP directory, before zip_entry_read allocates the whole
        // uncompressed payload: an entry that inflates to more than the limits
        // allow must never be inflated to find that out. A single entry over the
        // limit condemns the archive rather than being skipped - it is not a
        // damaged file among good ones, it is an archive built to be extracted
        // by something that does not look.
        const auto uncompressed = static_cast<UInt64>(zip_entry_size(archive));
        const auto compressed = static_cast<UInt64>(zip_entry_comp_size(archive));
        if (const auto reason = options.Limits.CheckEntry(uncompressed, compressed); !reason.empty())
        {
            PackageArchiverHelper::AddDiagnostic(result.Diagnostics, ToolSeverity::Error, std::string(reason), entryName);
            zip_entry_close(archive);
            zip_close(archive);
            return result;
        }

        if (compressed > std::numeric_limits<UInt64>::max() - compressedTotal ||
            uncompressed > std::numeric_limits<UInt64>::max() - uncompressedTotal)
        {
            PackageArchiverHelper::AddDiagnostic(result.Diagnostics, ToolSeverity::Error,
                                                 "ZIP package size accounting overflowed configured counters", entryName);
            zip_entry_close(archive);
            zip_close(archive);
            return result;
        }

        compressedTotal += compressed;
        uncompressedTotal += uncompressed;
        if (const auto reason = options.Limits.CheckTotals(compressedTotal, uncompressedTotal); !reason.empty())
        {
            PackageArchiverHelper::AddDiagnostic(result.Diagnostics, ToolSeverity::Error, std::string(reason), entryName);
            zip_entry_close(archive);
            zip_close(archive);
            return result;
        }

        void* buffer = nullptr;
        Size bufferSize = 0;
        const auto readSize = zip_entry_read(archive, &buffer, &bufferSize);
        zip_entry_close(archive);
        if (readSize < 0 || (buffer == nullptr && bufferSize > 0))
        {
            PackageArchiverHelper::AddDiagnostic(result.Diagnostics, ToolSeverity::Error, "Failed to read entry", entryName);
            continue;
        }

        std::vector<Byte> bytes;
        if (buffer != nullptr)
        {
            const auto* begin = static_cast<const UInt8*>(buffer);
            bytes.assign(begin, begin + bufferSize);
            std::free(buffer);
        }

        auto [diskPath, wasEscaped] = PackageArchiverHelper::MapEntryToDiskPath(entryName);
        if (wasEscaped)
        {
            renames.push_back(RenameEntry{entryName, PackageArchiverHelper::PathToPortableString(diskPath)});
        }

        if (options.PrettyPrintXml && PackageArchiverHelper::LooksLikeXml(entryName, bytes))
        {
            bool prettyOk = false;
            bytes = PackageArchiverHelper::PrettyPrintXmlBytes(bytes, prettyOk);
            anyPrettyPrinted = anyPrettyPrinted || prettyOk;
        }

        const auto fullPath = outDir / diskPath;
        std::filesystem::create_directories(fullPath.parent_path());
        std::ofstream file(fullPath, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            PackageArchiverHelper::AddDiagnostic(result.Diagnostics, ToolSeverity::Error, "Failed to write extracted entry",
                                                 fullPath.string());
            continue;
        }
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        ++result.EntryCount;
    }

    zip_close(archive);

    if (!renames.empty() || anyPrettyPrinted)
    {
        std::ostringstream manifest;
        manifest << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        manifest << "<exyokiUnpackManifest prettyPrinted=\"" << (anyPrettyPrinted ? "true" : "false") << "\">\n";
        for (const auto& rename : renames)
        {
            manifest << "  <rename from=\"" << PackageArchiverHelper::XmlEscape(rename.OriginalName) << "\" to=\""
                     << PackageArchiverHelper::XmlEscape(rename.DiskRelativePath) << "\"/>\n";
        }
        manifest << "</exyokiUnpackManifest>\n";

        std::ofstream manifestFile(outDir / std::filesystem::path(kArchiverManifestFileName), std::ios::trunc);
        manifestFile << manifest.str();
        result.ManifestWritten = true;
    }

    result.Ok = true;
    return result;
}

/// File-local manifest validation for Pack.
class PackageManifestHelper
{
public:
    /**
     * @brief True when a manifest may restore @p entryName as a ZIP entry name.
     *
     * The manifest is a file in the input directory, which means it is as
     * trustworthy as whoever produced that directory - and `Pack` is routinely
     * pointed at a tree someone else unpacked. Its `from` attribute becomes the
     * entry name verbatim, so an unchecked manifest turns `exyoki pack` into a
     * generator of traversal archives: harmless to `exyoki unpack`, which refuses
     * them, and not at all harmless to the next extractor down the line.
     *
     * The rule is the one Unpack applies in the other direction, so a name that
     * round-trips is accepted and a name that could not have come from an unpack is
     * not.
     */
    static bool IsRestorableEntryName(std::string_view entryName)
    {
        if (entryName.empty() || entryName.front() == '/' || entryName.front() == '\\')
        {
            return false;
        }

        if (entryName.find('\\') != std::string_view::npos || entryName.find(':') != std::string_view::npos)
        {
            return false;
        }

        const auto segments = PackageArchiverHelper::SplitEntryPath(entryName);
        if (segments.empty() || PackageArchiverHelper::HasTraversalSegment(segments))
        {
            return false;
        }

        return std::none_of(segments.begin(), segments.end(),
                            [](const std::string& segment)
                            { return segment == "."; });
    }

    static std::vector<std::pair<std::string, std::string>> LoadRenameManifest(const std::filesystem::path& inDir,
                                                                               std::vector<ToolDiagnostic>& diagnostics)
    {
        std::vector<std::pair<std::string, std::string>> renames;
        const auto manifestPath = inDir / std::filesystem::path(kArchiverManifestFileName);
        if (!std::filesystem::exists(manifestPath))
        {
            return renames;
        }

        std::ifstream file(manifestPath, std::ios::binary);
        std::ostringstream buffer;
        buffer << file.rdbuf();
        const auto xml = buffer.str();

        Pugi::xml_document doc;
        // load_buffer, not load_string: c_str() would end the document at the first
        // NUL byte, so a manifest carrying one would be parsed as its prefix and
        // silently accepted instead of rejected as malformed.
        if (!doc.load_buffer(xml.data(), xml.size(), Xml::ParseOptions::Preserving))
        {
            return renames;
        }
        auto root = doc.child("exyokiUnpackManifest");
        for (auto node = root.child("rename"); node; node = node.next_sibling("rename"))
        {
            const std::string diskRelative = node.attribute("to").as_string();
            const std::string originalName = node.attribute("from").as_string();
            if (!IsRestorableEntryName(originalName))
            {
                PackageArchiverHelper::AddDiagnostic(diagnostics, ToolSeverity::Warning,
                                                     "Ignored a manifest entry whose original name is not a usable ZIP entry name",
                                                     originalName);
                continue;
            }

            renames.emplace_back(diskRelative, originalName);
        }
        return renames;
    }

    static std::string ExtensionOf(const std::filesystem::path& path)
    {
        auto extension = path.extension().string();
        if (!extension.empty() && extension.front() == '.')
        {
            extension.erase(extension.begin());
        }
        return AsciiText::ToLower(extension);
    }

    static std::vector<Byte> BuildDefaultContentTypesXml(const std::vector<std::filesystem::path>& relativeFiles)
    {
        std::vector<std::string> extensions;
        for (const auto& file : relativeFiles)
        {
            auto extension = ExtensionOf(file);
            if (!extension.empty() && std::find(extensions.begin(), extensions.end(), extension) == extensions.end())
            {
                extensions.push_back(extension);
            }
        }
        std::sort(extensions.begin(), extensions.end());

        std::ostringstream output;
        output << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n";
        output << "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">";
        const auto& table = PackageArchiverHelper::DefaultContentTypeTable();
        for (const auto& extension : extensions)
        {
            const auto it = std::find_if(table.begin(), table.end(),
                                         [&](const auto& entry)
                                         { return entry.first == extension; });
            if (it != table.end())
            {
                output << "<Default Extension=\"" << extension << "\" ContentType=\"" << it->second << "\"/>";
            }
        }
        output << "</Types>";
        const auto text = output.str();
        return std::vector<Byte>(text.begin(), text.end());
    }
};

PackResult Pack(const std::filesystem::path& inDir, const std::filesystem::path& outPackage,
                const PackOptions& options)
{
    PackResult result;

    if (!std::filesystem::is_directory(inDir))
    {
        PackageArchiverHelper::AddDiagnostic(result.Diagnostics, ToolSeverity::Error, "Input directory does not exist", inDir.string());
        return result;
    }

    if (!options.Overwrite && std::filesystem::exists(outPackage))
    {
        PackageArchiverHelper::AddDiagnostic(result.Diagnostics, ToolSeverity::Error, "Output file already exists", outPackage.string());
        return result;
    }

    const auto renames = PackageManifestHelper::LoadRenameManifest(inDir, result.Diagnostics);
    auto resolveEntryName = [&](const std::filesystem::path& relative) -> std::string
    {
        const auto relativePortable = relative.generic_string();
        for (const auto& [diskRelative, originalName] : renames)
        {
            if (diskRelative == relativePortable)
            {
                return originalName;
            }
        }
        return relativePortable;
    };

    std::vector<std::filesystem::path> relativeFiles;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(inDir))
    {
        if (!entry.is_regular_file())
        {
            continue;
        }
        auto relative = std::filesystem::relative(entry.path(), inDir);
        if (relative == std::filesystem::path(kArchiverManifestFileName))
        {
            continue;
        }
        relativeFiles.push_back(relative);
    }
    std::sort(relativeFiles.begin(), relativeFiles.end());

    const int level = std::clamp(options.CompressionLevel, 0, 9);
    int error = 0;
    auto* archive = zip_openwitherror(outPackage.string().c_str(), level, 'w', &error);
    if (!archive)
    {
        PackageArchiverHelper::AddDiagnostic(result.Diagnostics, ToolSeverity::Error,
                                             std::string("Failed to create package: ") + zip_strerror(error), outPackage.string());
        return result;
    }

    const bool hasContentTypesOnDisk =
        std::find(relativeFiles.begin(), relativeFiles.end(), std::filesystem::path("[Content_Types].xml")) !=
        relativeFiles.end();
    const bool regenerate = options.RegenerateContentTypes || !hasContentTypesOnDisk;

    if (regenerate)
    {
        const auto bytes = PackageManifestHelper::BuildDefaultContentTypesXml(relativeFiles);
        if (zip_entry_open(archive, "[Content_Types].xml") == 0)
        {
            zip_entry_write(archive, bytes.data(), bytes.size());
            zip_entry_close(archive);
            ++result.EntryCount;
        }
        result.ContentTypesRegenerated = true;
    }

    for (const auto& relative : relativeFiles)
    {
        if (hasContentTypesOnDisk && regenerate && relative == std::filesystem::path("[Content_Types].xml"))
        {
            continue;
        }

        std::ifstream file(inDir / relative, std::ios::binary);
        std::vector<Byte> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        const auto entryName = resolveEntryName(relative);
        if (zip_entry_open(archive, entryName.c_str()) != 0)
        {
            PackageArchiverHelper::AddDiagnostic(result.Diagnostics, ToolSeverity::Error, "Failed to open ZIP entry for writing",
                                                 entryName);
            continue;
        }
        if (zip_entry_write(archive, bytes.data(), bytes.size()) < 0)
        {
            PackageArchiverHelper::AddDiagnostic(result.Diagnostics, ToolSeverity::Error, "Failed to write ZIP entry", entryName);
        }
        else
        {
            ++result.EntryCount;
        }
        zip_entry_close(archive);
    }

    zip_close(archive);
    result.Ok = true;

    if (options.ValidateAfterPack)
    {
        result.Validation = Run(outPackage);
    }

    return result;
}

} // namespace ExyokiOffice::Tools
