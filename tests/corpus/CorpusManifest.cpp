// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "CorpusManifest.hpp"

#include "ExyokiOffice/Security/CryptoProvider.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <stdexcept>

namespace ExyokiOfficeTests
{

/// File-local helpers. The repository avoids anonymous namespaces, so these sit
/// on a class with static members instead.
class CorpusManifestHelpers
{
public:
    static std::vector<CorpusDocument> Load()
    {
        const auto path = CorpusRoot() / "manifest.json";
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
        {
            throw std::runtime_error("Corpus manifest not found: " + path.string());
        }

        const auto json = nlohmann::json::parse(stream);
        if (json.at("FormatVersion").get<int>() != 1)
        {
            throw std::runtime_error("Unsupported corpus manifest format version.");
        }

        std::vector<CorpusDocument> documents;
        for (const auto& entry : json.at("Documents"))
        {
            CorpusDocument document;
            document.File = entry.at("File").get<std::string>();
            document.Family = entry.at("Family").get<std::string>();
            document.DocumentType = entry.at("DocumentType").get<std::string>();
            document.Producer = entry.at("Producer").get<std::string>();
            document.Purpose = entry.at("Purpose").get<std::string>();
            document.Features = entry.at("Features").get<std::vector<std::string>>();
            document.MainPart = entry.at("MainPart").get<std::string>();
            document.PartCount = entry.at("PartCount").get<ExyokiOffice::Size>();
            document.RelationshipCount = entry.at("RelationshipCount").get<ExyokiOffice::Size>();
            document.ContentTypeCount = entry.at("ContentTypeCount").get<ExyokiOffice::Size>();
            document.FileSize = entry.at("FileSize").get<ExyokiOffice::Size>();
            document.Sha256 = entry.at("Sha256").get<std::string>();
            document.RequiredParts = entry.at("RequiredParts").get<std::vector<std::string>>();
            documents.push_back(std::move(document));
        }

        if (documents.empty())
        {
            throw std::runtime_error("Corpus manifest lists no documents.");
        }
        return documents;
    }

    /// The value of @p name, or nothing when it is unset or empty.
    static std::optional<std::string> ReadEnvironmentVariable(const char* name)
    {
#ifdef _WIN32
        // getenv is deprecated under the MSVC CRT and this layer compiles with
        // warnings as errors.
        char* value = nullptr;
        size_t size = 0;
        if (_dupenv_s(&value, &size, name) != 0 || value == nullptr)
        {
            return std::nullopt;
        }

        std::string result(value);
        std::free(value);
#else
        const char* value = std::getenv(name);
        if (value == nullptr)
        {
            return std::nullopt;
        }

        std::string result(value);
#endif
        if (result.empty())
        {
            return std::nullopt;
        }
        return result;
    }

    static std::vector<CorpusDocument> LoadShard()
    {
        const auto& documents = CorpusManifest();
        const auto selected = ReadEnvironmentVariable(CorpusDocumentVariable);
        if (!selected)
        {
            return documents;
        }

        const auto found = std::find_if(documents.begin(), documents.end(),
                                        [&](const CorpusDocument& document)
                                        { return document.File == *selected; });
        if (found == documents.end())
        {
            throw std::runtime_error(std::string(CorpusDocumentVariable) + " names '" + *selected +
                                     "', which corpus/manifest.json does not list.");
        }

        return {*found};
    }
};

std::filesystem::path CorpusDocument::Path() const
{
    return CorpusRoot() / std::filesystem::path(File);
}

std::filesystem::path CorpusRoot()
{
    return std::filesystem::path(EXYOKIOFFICE_SOURCE_DIR) / "corpus";
}

const std::vector<CorpusDocument>& CorpusManifest()
{
    static const std::vector<CorpusDocument> documents = CorpusManifestHelpers::Load();
    return documents;
}

const std::vector<CorpusDocument>& CorpusShard()
{
    static const std::vector<CorpusDocument> documents = CorpusManifestHelpers::LoadShard();
    return documents;
}

std::vector<std::string> CorpusFilesOnDisk()
{
    std::vector<std::string> files;
    const auto root = CorpusRoot();
    if (!std::filesystem::exists(root))
    {
        return files;
    }

    for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
    {
        if (!entry.is_regular_file())
        {
            continue;
        }

        // The manifest itself and any editing leftovers are not fixtures.
        const auto extension = entry.path().extension().string();
        if (extension == ".json" || extension == ".md" || extension.empty())
        {
            continue;
        }
        files.push_back(std::filesystem::relative(entry.path(), root).generic_string());
    }

    std::sort(files.begin(), files.end());
    return files;
}

std::vector<ExyokiOffice::Byte> ReadAllBytes(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
    {
        return {};
    }

    const auto size = stream.tellg();
    if (size < 0)
    {
        return {};
    }

    std::vector<ExyokiOffice::Byte> bytes(static_cast<std::size_t>(size));
    stream.seekg(0);
    if (!bytes.empty())
    {
        stream.read(reinterpret_cast<char*>(bytes.data()), size);
    }
    return stream.good() || stream.eof() ? bytes : std::vector<ExyokiOffice::Byte>{};
}

std::string Sha256Hex(std::span<const ExyokiOffice::Byte> bytes)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    const auto digest = ExyokiOffice::Security::ComputeDigest(ExyokiOffice::Security::DigestAlgorithm::Sha256, bytes);

    std::string result;
    result.reserve(digest.size() * 2);
    for (const auto byte : digest)
    {
        result.push_back(kDigits[byte >> 4]);
        result.push_back(kDigits[byte & 0x0FU]);
    }
    return result;
}

} // namespace ExyokiOfficeTests
