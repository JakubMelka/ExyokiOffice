// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Tools/FlatOpcConverter.hpp"

#include "pugixml/pugixml.hpp"
#include "zip/zip.h"
#include "XmlParseOptions.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace ExyokiOffice::Tools
{

class FlatOpcHelpers
{
public:
    static constexpr std::string_view Namespace = "http://schemas.microsoft.com/office/2006/xmlPackage";
    static constexpr std::string_view ContentTypesType =
        "application/vnd.openxmlformats-package.content-types+xml";

    static void Diagnostic(std::vector<ToolDiagnostic>& diagnostics, std::string message, std::string context = {})
    {
        diagnostics.push_back({ToolSeverity::Error, std::move(message), std::move(context)});
    }

    static std::vector<Byte> ReadEntry(zip_t* archive, bool& ok)
    {
        void* buffer = nullptr;
        Size size = 0;
        const auto read = zip_entry_read(archive, &buffer, &size);
        ok = read >= 0 && (buffer != nullptr || size == 0);
        std::vector<Byte> bytes;
        if (ok && buffer)
        {
            const auto* first = static_cast<const UInt8*>(buffer);
            bytes.assign(first, first + size);
        }
        std::free(buffer);
        return bytes;
    }

    static std::string Lower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char ch)
                       { return static_cast<char>(std::tolower(ch)); });
        return value;
    }

    static std::string Extension(std::string_view name)
    {
        const auto slash = name.find_last_of('/');
        const auto dot = name.find_last_of('.');
        if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash))
        {
            return {};
        }
        return Lower(std::string(name.substr(dot + 1)));
    }

    static void LoadContentTypes(const std::vector<Byte>& bytes,
                                 std::unordered_map<std::string, std::string>& defaults,
                                 std::unordered_map<std::string, std::string>& overrides)
    {
        Pugi::xml_document doc;
        if (!doc.load_buffer(bytes.data(), bytes.size(), Xml::ParseOptions::Preserving))
        {
            return;
        }
        const auto root = doc.document_element();
        for (auto node : root.children())
        {
            const std::string_view name = node.name();
            const auto local = name.substr(name.find(':') == std::string_view::npos ? 0 : name.find(':') + 1);
            if (local == "Default")
            {
                defaults[Lower(node.attribute("Extension").as_string())] = node.attribute("ContentType").as_string();
            }
            else if (local == "Override")
            {
                overrides[node.attribute("PartName").as_string()] = node.attribute("ContentType").as_string();
            }
        }
    }

    static std::string EncodeBase64(const std::vector<Byte>& bytes)
    {
        static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve((bytes.size() + 2) / 3 * 4);
        for (Size i = 0; i < bytes.size(); i += 3)
        {
            const unsigned value = (static_cast<unsigned>(bytes[i]) << 16) |
                                   (i + 1 < bytes.size() ? static_cast<unsigned>(bytes[i + 1]) << 8 : 0) |
                                   (i + 2 < bytes.size() ? bytes[i + 2] : 0);
            out.push_back(alphabet[(value >> 18) & 63]);
            out.push_back(alphabet[(value >> 12) & 63]);
            out.push_back(i + 1 < bytes.size() ? alphabet[(value >> 6) & 63] : '=');
            out.push_back(i + 2 < bytes.size() ? alphabet[value & 63] : '=');
        }
        return out;
    }

    static bool DecodeBase64(std::string_view text, std::vector<Byte>& out)
    {
        int value = 0, bits = -8;
        bool padding = false;
        for (const char rawCh : text)
        {
            const auto ch = static_cast<unsigned char>(rawCh);
            if (std::isspace(ch))
            {
                continue;
            }
            if (ch == '=')
            {
                padding = true;
                continue;
            }
            if (padding)
            {
                return false;
            }
            int digit = ch >= 'A' && ch <= 'Z' ? ch - 'A' : ch >= 'a' && ch <= 'z' ? ch - 'a' + 26
                                                        : ch >= '0' && ch <= '9'   ? ch - '0' + 52
                                                        : ch == '+'                ? 62
                                                        : ch == '/'                ? 63
                                                                                   : -1;
            if (digit < 0)
            {
                return false;
            }
            value = (value << 6) | digit;
            bits += 6;
            if (bits >= 0)
            {
                out.push_back(static_cast<UInt8>((value >> bits) & 255));
                bits -= 8;
            }
        }
        return true;
    }

    static std::string Prefix(Pugi::xml_node root)
    {
        for (auto attribute : root.attributes())
        {
            const std::string_view name = attribute.name();
            if (name.starts_with("xmlns:") && attribute.value() == Namespace)
            {
                return std::string(name.substr(6));
            }
        }
        return "pkg";
    }
};

ToFlatOpcResult ConvertToFlatOpc(std::span<const Byte> packageBytes, const ToFlatOpcOptions& options)
{
    ToFlatOpcResult result;
    auto* archive = zip_stream_open(reinterpret_cast<const char*>(packageBytes.data()), packageBytes.size(), 0, 'r');
    if (!archive)
    {
        FlatOpcHelpers::Diagnostic(result.Diagnostics, "Failed to open in-memory OPC package");
        return result;
    }

    std::unordered_map<std::string, std::string> defaults, overrides;
    if (zip_entry_open(archive, "[Content_Types].xml") == 0)
    {
        bool ok = false;
        const auto bytes = FlatOpcHelpers::ReadEntry(archive, ok);
        zip_entry_close(archive);
        if (ok)
        {
            FlatOpcHelpers::LoadContentTypes(bytes, defaults, overrides);
        }
    }

    Pugi::xml_document flat;
    auto declaration = flat.append_child(Pugi::node_declaration);
    declaration.append_attribute("version") = "1.0";
    declaration.append_attribute("encoding") = "UTF-8";
    declaration.append_attribute("standalone") = "yes";
    auto root = flat.append_child("pkg:package");
    root.append_attribute("xmlns:pkg") = FlatOpcHelpers::Namespace.data();

    const auto total = zip_entries_total(archive);
    for (ssize_t index = 0; index < total; ++index)
    {
        if (zip_entry_openbyindex(archive, static_cast<Size>(index)) != 0)
        {
            continue;
        }
        const std::string name = zip_entry_name(archive) ? zip_entry_name(archive) : "";
        if (zip_entry_isdir(archive) || name.empty())
        {
            zip_entry_close(archive);
            continue;
        }
        if (name.find("..") != std::string::npos)
        {
            FlatOpcHelpers::Diagnostic(result.Diagnostics, "Rejected entry with path traversal", name);
            zip_entry_close(archive);
            continue;
        }
        bool readOk = false;
        const auto bytes = FlatOpcHelpers::ReadEntry(archive, readOk);
        zip_entry_close(archive);
        if (!readOk)
        {
            FlatOpcHelpers::Diagnostic(result.Diagnostics, "Failed to read entry", name);
            continue;
        }

        const std::string partName = "/" + name;
        std::string contentType;
        if (name == "[Content_Types].xml")
        {
            contentType = std::string(FlatOpcHelpers::ContentTypesType);
        }
        else if (const auto overrideIt = overrides.find(partName); overrideIt != overrides.end())
        {
            contentType = overrideIt->second;
        }
        else if (const auto defaultIt = defaults.find(FlatOpcHelpers::Extension(name)); defaultIt != defaults.end())
        {
            contentType = defaultIt->second;
        }
        else
        {
            contentType = "application/octet-stream";
        }

        auto part = root.append_child("pkg:part");
        part.append_attribute("pkg:name") = partName.c_str();
        part.append_attribute("pkg:contentType") = contentType.c_str();
        Pugi::xml_document xml;
        if (xml.load_buffer(bytes.data(), bytes.size(), Xml::ParseOptions::Preserving) && xml.document_element())
        {
            auto data = part.append_child("pkg:xmlData");
            data.append_copy(xml.document_element());
        }
        else
        {
            part.append_child("pkg:binaryData").text().set(FlatOpcHelpers::EncodeBase64(bytes).c_str());
        }
        ++result.PartCount;
    }
    zip_stream_close(archive);

    std::ostringstream output;
    flat.save(output, "  ", options.PrettyPrint ? Pugi::format_indent : Pugi::format_raw, Pugi::encoding_utf8);
    result.FlatOpcXml = output.str();
    result.Ok = true;
    return result;
}

ToFlatOpcResult ConvertToFlatOpc(const std::filesystem::path& packagePath, const std::filesystem::path& outFile,
                                 const ToFlatOpcOptions& options)
{
    std::ifstream input(packagePath, std::ios::binary);
    if (!input)
    {
        ToFlatOpcResult result;
        FlatOpcHelpers::Diagnostic(result.Diagnostics, "Failed to read package", packagePath.string());
        return result;
    }
    std::vector<Byte> bytes((std::istreambuf_iterator<char>(input)), {});
    auto result = ConvertToFlatOpc(bytes, options);
    if (result.Ok)
    {
        std::ofstream output(outFile, std::ios::binary | std::ios::trunc);
        if (!output || !output.write(result.FlatOpcXml.data(), static_cast<std::streamsize>(result.FlatOpcXml.size())))
        {
            result.Ok = false;
            FlatOpcHelpers::Diagnostic(result.Diagnostics, "Failed to write Flat OPC file", outFile.string());
        }
    }
    return result;
}

FromFlatOpcResult ConvertFromFlatOpc(std::string_view flatOpcXml, const FromFlatOpcOptions& options)
{
    FromFlatOpcResult result;
    Pugi::xml_document flat;
    const auto parsed = flat.load_buffer(flatOpcXml.data(), flatOpcXml.size(), Xml::ParseOptions::Preserving);
    if (!parsed)
    {
        FlatOpcHelpers::Diagnostic(result.Diagnostics, std::string("Failed to parse Flat OPC XML: ") + parsed.description());
        return result;
    }
    const auto root = flat.document_element();
    const auto prefix = FlatOpcHelpers::Prefix(root);
    const auto qualified = [&](std::string_view local)
    { return prefix + ":" + std::string(local); };
    if (std::string(root.name()) != qualified("package"))
    {
        FlatOpcHelpers::Diagnostic(result.Diagnostics, "Flat OPC package root was not found");
        return result;
    }

    auto* archive = zip_stream_open(nullptr, 0, std::clamp(options.CompressionLevel, 0, 9), 'w');
    if (!archive)
    {
        FlatOpcHelpers::Diagnostic(result.Diagnostics, "Failed to create in-memory OPC package");
        return result;
    }
    for (auto part = root.child(qualified("part").c_str()); part; part = part.next_sibling(qualified("part").c_str()))
    {
        std::string entryName = part.attribute(qualified("name").c_str()).as_string();
        if (!entryName.empty() && entryName.front() == '/')
        {
            entryName.erase(entryName.begin());
        }
        if (entryName.empty() || entryName.find("..") != std::string::npos)
        {
            FlatOpcHelpers::Diagnostic(result.Diagnostics, "Rejected invalid Flat OPC part name", entryName);
            continue;
        }
        std::vector<Byte> bytes;
        if (auto xmlData = part.child(qualified("xmlData").c_str()))
        {
            const auto element = xmlData.first_child();
            if (!element || element.type() != Pugi::node_element)
            {
                FlatOpcHelpers::Diagnostic(result.Diagnostics, "XML part has no document element", entryName);
                continue;
            }
            std::ostringstream output;
            output << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n";
            element.print(output, "", Pugi::format_raw, Pugi::encoding_utf8);
            const auto text = output.str();
            bytes.assign(text.begin(), text.end());
        }
        else if (auto binaryData = part.child(qualified("binaryData").c_str()))
        {
            if (!FlatOpcHelpers::DecodeBase64(binaryData.text().as_string(), bytes))
            {
                FlatOpcHelpers::Diagnostic(result.Diagnostics, "Invalid base64 data", entryName);
                continue;
            }
        }
        else
        {
            FlatOpcHelpers::Diagnostic(result.Diagnostics, "Part contains neither XML nor binary data", entryName);
            continue;
        }
        if (zip_entry_open(archive, entryName.c_str()) != 0)
        {
            FlatOpcHelpers::Diagnostic(result.Diagnostics, "Failed to write package entry", entryName);
            continue;
        }
        const bool writeOk = zip_entry_write(archive, bytes.data(), bytes.size()) == 0;
        const bool closeOk = zip_entry_close(archive) == 0;
        if (!writeOk || !closeOk)
        {
            FlatOpcHelpers::Diagnostic(result.Diagnostics, "Failed to write package entry", entryName);
            continue;
        }
        ++result.PartCount;
    }
    void* buffer = nullptr;
    Size size = 0;
    if (zip_stream_copy(archive, &buffer, &size) < 0 || (!buffer && size > 0))
    {
        FlatOpcHelpers::Diagnostic(result.Diagnostics, "Failed to finalize in-memory OPC package");
    }
    else
    {
        const auto* first = static_cast<const UInt8*>(buffer);
        result.PackageBytes.assign(first, first + size);
        result.Ok = true;
    }
    std::free(buffer);
    zip_stream_close(archive);
    return result;
}

FromFlatOpcResult ConvertFromFlatOpc(const std::filesystem::path& flatOpcPath,
                                     const std::filesystem::path& outPackage,
                                     const FromFlatOpcOptions& options)
{
    std::ifstream input(flatOpcPath, std::ios::binary);
    if (!input)
    {
        FromFlatOpcResult result;
        FlatOpcHelpers::Diagnostic(result.Diagnostics, "Failed to read Flat OPC file", flatOpcPath.string());
        return result;
    }
    std::ostringstream content;
    content << input.rdbuf();
    auto result = ConvertFromFlatOpc(content.str(), options);
    if (result.Ok)
    {
        std::ofstream output(outPackage, std::ios::binary | std::ios::trunc);
        if (!output || !output.write(reinterpret_cast<const char*>(result.PackageBytes.data()),
                                     static_cast<std::streamsize>(result.PackageBytes.size())))
        {
            result.Ok = false;
            FlatOpcHelpers::Diagnostic(result.Diagnostics, "Failed to write package", outPackage.string());
        }
    }
    return result;
}

} // namespace ExyokiOffice::Tools
