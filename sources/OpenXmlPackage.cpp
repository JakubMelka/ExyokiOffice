// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/OpenXmlPackagePart.hpp"
#include "ExyokiOffice/Packaging/OpenXmlPackageFactory.hpp"

#include "ExyokiOffice/Security/PackageSignatures.hpp"

#include "NativePath.hpp"
#include "OpenXmlPackageInternal.hpp"
#include "OpenXmlPackageUri.hpp"
#include "Security/SignatureNames.hpp"
#include "XmlParseOptions.hpp"
#include "pugixml/pugixml.hpp"
#include "zip/zip.h"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstring>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <limits>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <utility>
#include <vector>

namespace ExyokiOffice
{

/// File-local helpers for package loading and saving.
class OpenXmlPackageHelper
{
public:
    static constexpr std::string_view kContentTypesEntry = "[Content_Types].xml";
    static constexpr std::string_view kRelationshipsExtension = ".rels";
    static constexpr std::string_view kRelationshipsContentType =
        "application/vnd.openxmlformats-package.relationships+xml";

    static bool IsCancellationRequested(const ICancellationToken* cancellationToken)
    {
        return cancellationToken != nullptr && cancellationToken->IsCancelled();
    }

    static bool EndsWith(std::string_view value, std::string_view suffix)
    {
        return value.size() >= suffix.size() && value.substr(value.size() - suffix.size(), suffix.size()) == suffix;
    }

    static std::string_view TrimLeadingSlash(std::string_view path)
    {
        if (!path.empty() && path.front() == '/')
        {
            return path.substr(1);
        }
        return path;
    }

    static std::string NormalizeEntryName(std::string_view name)
    {
        std::string normalized;
        normalized.reserve(name.size());
        for (char ch : name)
        {
            if (ch == '\\')
            {
                normalized.push_back('/');
            }
            else
            {
                normalized.push_back(ch);
            }
        }
        return normalized;
    }

    struct ZipEntryData
    {
        std::string Name;
        std::vector<Byte> Data;
    };
};

class OpenXmlPackageFileSave
{
public:
    static std::filesystem::path BuildTemporaryPackagePath(const std::filesystem::path& targetPath)
    {
        const auto directory = targetPath.parent_path();
        const auto filename = targetPath.filename().empty()
                                  ? std::filesystem::path("package")
                                  : targetPath.filename();

        std::random_device randomDevice;
        std::mt19937_64 generator(randomDevice());
        std::uniform_int_distribution<UInt64> distribution;

        for (UInt32 attempt = 0; attempt < 100; ++attempt)
        {
            std::ostringstream suffix;
            suffix << std::hex << std::setw(16) << std::setfill('0') << distribution(generator);

            auto candidate = directory / filename;
            candidate += ".tmp.";
            candidate += suffix.str();

            std::error_code ec;
            if (!std::filesystem::exists(candidate, ec) && !ec)
            {
                return candidate;
            }
        }

        return {};
    }

    static bool RemoveFileIfPresent(const std::filesystem::path& path)
    {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec))
        {
            return !ec;
        }
        std::filesystem::remove(path, ec);
        return !ec;
    }
};

/// Storage of the process-wide default behind OpenXmlPackage::SetDefaultPackageLimits.
class OpenXmlPackageDefaultLimits
{
public:
    static void Set(std::optional<OpenXmlPackageLimits> limits)
    {
        const std::lock_guard<std::mutex> guard(Mutex());
        Value() = limits;
    }

    static std::optional<OpenXmlPackageLimits> Get()
    {
        const std::lock_guard<std::mutex> guard(Mutex());
        return Value();
    }

private:
    static std::mutex& Mutex()
    {
        static std::mutex mutex;
        return mutex;
    }

    /// Empty until an application sets one, which is what lets a layer above
    /// tell a deliberate Unlimited() apart from "nobody chose".
    static std::optional<OpenXmlPackageLimits>& Value()
    {
        static std::optional<OpenXmlPackageLimits> limits;
        return limits;
    }
};

void OpenXmlPackage::SetDefaultPackageLimits(std::optional<OpenXmlPackageLimits> limits)
{
    OpenXmlPackageDefaultLimits::Set(limits);
}

OpenXmlPackageLimits OpenXmlPackage::DefaultPackageLimits()
{
    return OpenXmlPackageDefaultLimits::Get().value_or(OpenXmlPackageLimits::Recommended());
}

std::optional<OpenXmlPackageLimits> OpenXmlPackage::ConfiguredDefaultPackageLimits()
{
    return OpenXmlPackageDefaultLimits::Get();
}

OpenXmlPackage::OpenXmlPackage()
    : OpenXmlPartContainer(this), m_impl(std::make_unique<OpenXmlPackageImpl>())
{
    // Applied here rather than at load time so that a caller who sets its own
    // limits with SetPackageLimits always wins, whichever order the two happen
    // in: the explicit call comes after construction by definition.
    m_impl->packageLimits = OpenXmlPackageDefaultLimits::Get().value_or(OpenXmlPackageLimits::Recommended());
}

OpenXmlPackage::~OpenXmlPackage()
{
    Clear();
}

bool OpenXmlPackage::LoadFromFile(const std::filesystem::path& path, const ICancellationToken* cancellationToken)
{
    if (OpenXmlPackageHelper::IsCancellationRequested(cancellationToken))
    {
        return false;
    }

    Clear();
    int error = 0;
    auto archive = zip_openwitherror(NativePath::ToUtf8(path).c_str(), 0, 'r', &error);
    if (!archive)
    {
        return false;
    }
    const bool result = m_impl->LoadFromZip(*this, archive, cancellationToken);
    zip_close(archive);
    if (!result)
    {
        ClearPackageState(true);
        return false;
    }
    m_impl->packagePath = path;
    return result;
}

bool OpenXmlPackage::SaveToFile(const std::filesystem::path& path,
                                bool atomicSave,
                                const ICancellationToken* cancellationToken)
{
    if (OpenXmlPackageHelper::IsCancellationRequested(cancellationToken))
    {
        return false;
    }
    if (!BeforeSave())
    {
        return false;
    }
    if (!ApplySignatureSavePolicy())
    {
        return false;
    }
    if (OpenXmlPackageHelper::IsCancellationRequested(cancellationToken))
    {
        return false;
    }

    const bool writeAtomically = atomicSave || cancellationToken != nullptr;
    const auto outputPath = writeAtomically ? OpenXmlPackageFileSave::BuildTemporaryPackagePath(path) : path;
    if (outputPath.empty())
    {
        return false;
    }

    int error = 0;
    auto archive =
        zip_openwitherror(NativePath::ToUtf8(outputPath).c_str(), ZIP_DEFAULT_COMPRESSION_LEVEL, 'w', &error);
    if (!archive)
    {
        return false;
    }
    const bool result = m_impl->SaveToZip(*this, archive, cancellationToken);
    zip_close(archive);
    if (!result || OpenXmlPackageHelper::IsCancellationRequested(cancellationToken))
    {
        if (writeAtomically)
        {
            OpenXmlPackageFileSave::RemoveFileIfPresent(outputPath);
        }
        return false;
    }

    if (!writeAtomically)
    {
        return true;
    }

    std::error_code ec;
    std::filesystem::rename(outputPath, path, ec);
    if (!ec)
    {
        return true;
    }

    OpenXmlPackageFileSave::RemoveFileIfPresent(outputPath);
    return false;
}

bool OpenXmlPackage::LoadFromMemory(std::span<const Byte> buffer, const ICancellationToken* cancellationToken)
{
    if (OpenXmlPackageHelper::IsCancellationRequested(cancellationToken))
    {
        return false;
    }

    Clear();
    int error = 0;
    auto archive = zip_stream_openwitherror(reinterpret_cast<const char*>(buffer.data()),
                                            buffer.size(),
                                            0,
                                            'r',
                                            &error);
    if (!archive)
    {
        return false;
    }
    const bool result = m_impl->LoadFromZip(*this, archive, cancellationToken);
    zip_stream_close(archive);
    if (!result)
    {
        ClearPackageState(true);
    }
    return result;
}

std::vector<Byte> OpenXmlPackage::SaveToMemory(const ICancellationToken* cancellationToken)
{
    std::vector<Byte> buffer;

    if (OpenXmlPackageHelper::IsCancellationRequested(cancellationToken))
    {
        return buffer;
    }
    if (!BeforeSave())
    {
        return buffer;
    }
    if (!ApplySignatureSavePolicy())
    {
        return buffer;
    }
    if (OpenXmlPackageHelper::IsCancellationRequested(cancellationToken))
    {
        return buffer;
    }

    auto archive = zip_stream_open(nullptr, 0, ZIP_DEFAULT_COMPRESSION_LEVEL, 'w');
    if (!archive)
    {
        return buffer;
    }
    if (!m_impl->SaveToZip(*this, archive, cancellationToken) || OpenXmlPackageHelper::IsCancellationRequested(cancellationToken))
    {
        zip_stream_close(archive);
        return buffer;
    }
    void* outBuffer = nullptr;
    Size outSize = 0;
    if (zip_stream_copy(archive, &outBuffer, &outSize) > 0 && outBuffer && outSize > 0)
    {
        buffer.resize(outSize);
        std::memcpy(buffer.data(), outBuffer, outSize);
        zip_stream_close(archive);
        std::free(outBuffer);
        return buffer;
    }
    zip_stream_close(archive);
    return buffer;
}

std::shared_ptr<OpenXmlPackagePart> OpenXmlPackage::GetPartByUri(std::string_view uri) const
{
    const auto normalized = m_impl->NormalizePartUriStringView(uri);
    if (auto it = m_impl->partsByUri.find(normalized); it != m_impl->partsByUri.end())
    {
        return it->second;
    }
    return nullptr;
}

const ValidationResult& OpenXmlPackage::LastValidationResult() const noexcept
{
    return m_impl->lastValidationResult;
}

OpenXmlPackageLimits OpenXmlPackageLimits::Recommended() noexcept
{
    constexpr UInt64 kMiB = 1024ull * 1024ull;

    OpenXmlPackageLimits limits;
    // Office packages run to a few hundred parts even when they carry a lot of
    // media; four figures is already generous.
    limits.MaxEntries = 10'000;
    limits.MaxCompressedBytes = 256ull * kMiB;
    limits.MaxUncompressedBytes = 2048ull * kMiB;
    limits.MaxPartBytes = 512ull * kMiB;
    limits.MaxRelationships = 65'536;
    // OOXML is repetitive and compresses well - 20x to 50x is ordinary and
    // pathological-but-real documents reach about 100x. Decompression bombs
    // start around 1000x, so 200 separates them without false positives.
    limits.MaxCompressionRatio = 200;
    // Nesting is shallow in practice; even deeply nested tables stay well under
    // a hundred levels.
    limits.MaxXmlDepth = 256;
    // Per part. A worksheet with millions of populated cells is the case that
    // sets the scale here; MaxPartBytes binds first for anything larger.
    limits.MaxXmlNodes = 50'000'000;
    limits.MaxXmlAttributes = 50'000'000;
    limits.MaxXmlTextCharacters = 512ull * kMiB;
    return limits;
}

OpenXmlPackageLimits OpenXmlPackageLimits::Unlimited() noexcept
{
    return OpenXmlPackageLimits{};
}

std::string_view OpenXmlPackageLimits::CheckEntry(UInt64 uncompressedBytes, UInt64 compressedBytes) const noexcept
{
    if (MaxPartBytes != 0 && uncompressedBytes > MaxPartBytes)
    {
        return "ZIP entry exceeds configured part size limit";
    }

    if (MaxCompressionRatio != 0 && uncompressedBytes > 0)
    {
        // A zero compressed size with a non-zero uncompressed one is not a
        // ratio that can be computed, and no honest entry looks like that.
        if (compressedBytes == 0)
        {
            return "ZIP entry exceeds configured compression ratio limit";
        }

        // Multiply rather than divide: integer division truncates, so 2001
        // bytes out of 10 reads as exactly 200 and slips past a limit of 200
        // even though the real ratio is 200.1. Entries too large to scale
        // without overflow cannot exceed the ratio - the product would already
        // be past every representable uncompressed size - so they are not
        // rejected here; the size limits above cover them.
        const UInt64 scalable = std::numeric_limits<UInt64>::max() / MaxCompressionRatio;
        if (compressedBytes <= scalable && uncompressedBytes > compressedBytes * MaxCompressionRatio)
        {
            return "ZIP entry exceeds configured compression ratio limit";
        }
    }

    return {};
}

std::string_view OpenXmlPackageLimits::CheckEntryCount(UInt64 entryCount) const noexcept
{
    if (MaxEntries != 0 && entryCount > MaxEntries)
    {
        return "ZIP package exceeds configured entry count limit";
    }

    return {};
}

std::string_view OpenXmlPackageLimits::CheckTotals(UInt64 compressedTotal, UInt64 uncompressedTotal) const noexcept
{
    if (MaxCompressedBytes != 0 && compressedTotal > MaxCompressedBytes)
    {
        return "ZIP package exceeds configured compressed size limit";
    }

    if (MaxUncompressedBytes != 0 && uncompressedTotal > MaxUncompressedBytes)
    {
        return "ZIP package exceeds configured uncompressed size limit";
    }

    return {};
}

void OpenXmlPackage::SetPackageLimits(OpenXmlPackageLimits limits)
{
    m_impl->packageLimits = limits;
}

const OpenXmlPackageLimits& OpenXmlPackage::PackageLimits() const noexcept
{
    return m_impl->packageLimits;
}

void OpenXmlPackage::SetPartByteRetention(PartByteRetention retention) noexcept
{
    m_impl->partByteRetention = retention;
}

PartByteRetention OpenXmlPackage::GetPartByteRetention() const noexcept
{
    return m_impl->partByteRetention;
}

void OpenXmlPackage::SetSignatureSavePolicy(SignatureSavePolicy policy) noexcept
{
    m_impl->signatureSavePolicy = policy;
}

SignatureSavePolicy OpenXmlPackage::GetSignatureSavePolicy() const noexcept
{
    return m_impl->signatureSavePolicy;
}

void OpenXmlPackage::SuppressSaveTimePropertyUpdateOnce() noexcept
{
    m_impl->suppressSaveTimePropertyUpdate = true;
}

bool OpenXmlPackage::IsSaveTimePropertyUpdateSuppressed() const noexcept
{
    return m_impl->suppressSaveTimePropertyUpdate;
}

bool OpenXmlPackage::ConsumeSaveTimePropertyUpdateSuppression() noexcept
{
    return std::exchange(m_impl->suppressSaveTimePropertyUpdate, false);
}

void OpenXmlPackage::SetExternalResourceResolver(Security::IExternalResourceResolver::Ptr resolver)
{
    m_impl->externalResourceResolver = std::move(resolver);
}

const Security::IExternalResourceResolver::Ptr& OpenXmlPackage::GetExternalResourceResolver() const noexcept
{
    return m_impl->externalResourceResolver;
}

void OpenXmlPackage::SetExternalResourcePolicy(Security::ExternalResourcePolicy policy)
{
    m_impl->externalResourcePolicy = std::move(policy);
}

const Security::ExternalResourcePolicy& OpenXmlPackage::GetExternalResourcePolicy() const noexcept
{
    return m_impl->externalResourcePolicy;
}

void OpenXmlPackage::ResetExternalResourceBudget() noexcept
{
    m_impl->externalRequestCount = 0;
    m_impl->externalBytesUsed = 0;
}

bool OpenXmlPackage::ApplySignatureSavePolicy()
{
    if (m_impl->signatureSavePolicy == SignatureSavePolicy::Ignore || !Security::HasSignatures(*this))
    {
        return true;
    }

    if (m_impl->signatureSavePolicy == SignatureSavePolicy::RemoveSignatures)
    {
        Security::RemoveAllSignatures(*this);
        return true;
    }

    // Recompute the signed digests against the bytes this save is about to
    // write, so the answer is exact rather than a guess based on edit tracking.
    const auto check = Security::CheckSignaturesBeforeSave(*this);
    if (check.AllContentIntact())
    {
        return true;
    }

    for (const auto& signature : check.Signatures)
    {
        if (signature.ContentIntegrity == Security::SignatureCheck::Valid)
        {
            continue;
        }

        ValidationIssue issue;
        issue.Severity = ValidationSeverity::Warning;
        issue.Domain = ValidationDomain::Security;
        issue.Id = ValidationErrorId::SignatureInvalidatedBySave;
        issue.Message = "Saving invalidates the digital signature in " + signature.PartUri +
                        ". Signatures cover the bytes a part was stored with, and saving writes the parts anew.";
        issue.PartUri = signature.PartUri;
        m_impl->lastValidationResult.AddIssue(std::move(issue));
    }

    return m_impl->signatureSavePolicy != SignatureSavePolicy::FailSave;
}

void OpenXmlPackage::Clear()
{
    ClearPackageState(false);
}

void OpenXmlPackage::ClearPackageState(bool preserveValidationResult)
{
    // Before the root container and the registry let go of their references, and
    // while the graph can still be walked from the root.
    m_impl->ReleaseAllPartOwnership(*this);
    ResetContainerState();
    m_impl->partsByUri.clear();
    m_impl->contentTypeOverrides.clear();
    m_impl->defaultContentTypes.clear();
    m_impl->duplicatePartUris.clear();
    if (!preserveValidationResult)
    {
        m_impl->lastValidationResult = {};
    }
    m_impl->packagePath.clear();
    // The budget belongs to the content, not to the object: a package that is
    // cleared or reloaded starts over. The resolver and the policy are settings
    // of the object and deliberately survive.
    m_impl->externalRequestCount = 0;
    m_impl->externalBytesUsed = 0;
}

std::filesystem::path OpenXmlPackage::ContainerPath() const
{
    return std::filesystem::path("/");
}

std::string OpenXmlPackage::ContainerUri() const
{
    return "/";
}

void OpenXmlPackage::AttachRootPart(const std::shared_ptr<OpenXmlPackagePart>& part,
                                    const OpenXmlPartDescriptor& descriptor,
                                    bool allowMultiple)
{
    AttachChildPart(part, descriptor, allowMultiple);
}

bool OpenXmlPackage::DetachRootPart(const std::shared_ptr<OpenXmlPackagePart>& part)
{
    return DetachChildPart(part);
}

void OpenXmlPackage::RegisterPart(const std::shared_ptr<OpenXmlPackagePart>& part)
{
    if (!part)
    {
        return;
    }
    m_impl->partsByUri[part->Uri()] = part;
}

void OpenXmlPackage::UnregisterPart(const std::string& uri)
{
    m_impl->partsByUri.erase(uri);
}

bool OpenXmlPackage::IsPartUriInUse(const std::string& uri) const
{
    return m_impl->partsByUri.find(uri) != m_impl->partsByUri.end();
}

void OpenXmlPackage::ClearValidationResult()
{
    m_impl->lastValidationResult = {};
}

void OpenXmlPackage::SetLastValidationResult(ValidationResult result)
{
    m_impl->lastValidationResult = std::move(result);
}

void OpenXmlPackageImpl::RemoveUnreachableParts(const OpenXmlPackage& self)
{
    std::unordered_set<std::string> reachableUris;

    const std::function<void(const OpenXmlPartContainer&)> visitContainer = [&](const OpenXmlPartContainer& container)
    {
        for (const auto& child : container.Parts())
        {
            if (!child || child->Uri().empty())
            {
                continue;
            }
            if (!reachableUris.insert(child->Uri()).second)
            {
                continue;
            }
            visitContainer(*child);
        }
    };

    visitContainer(self);

    std::unordered_set<std::string> removedUris;
    for (const auto& [uri, part] : partsByUri)
    {
        if (reachableUris.find(uri) == reachableUris.end())
        {
            removedUris.insert(uri);
        }
    }

    if (removedUris.empty())
    {
        return;
    }

    // A part that leaves the package can be the target of another part leaving with
    // it, and in a presentation that happens in both directions at once - a slide
    // master owns its layouts and every layout owns its master back. Those edges
    // have to be cut, or the component keeps itself alive with nothing left to
    // reach it. Edges pointing at surviving parts are left alone: a survivor is
    // reachable from the root by definition, so it can never close a cycle with a
    // part that is being removed.
    const std::function<bool(const OpenXmlPackagePart&)> isRemoved =
        [&removedUris](const OpenXmlPackagePart& candidate)
    {
        return removedUris.find(candidate.Uri()) != removedUris.end();
    };

    for (const auto& uri : removedUris)
    {
        const auto it = partsByUri.find(uri);
        if (it != partsByUri.end() && it->second)
        {
            it->second->ReleaseChildParts(isRemoved);
        }
    }

    for (auto it = partsByUri.begin(); it != partsByUri.end();)
    {
        if (removedUris.find(it->first) == removedUris.end())
        {
            ++it;
            continue;
        }

        if (it->second)
        {
            it->second->ClearIncomingRelationships();
            it->second->SetPackage(nullptr);
        }
        it = partsByUri.erase(it);
    }
}

void OpenXmlPackageImpl::ReleaseAllPartOwnership(const OpenXmlPackage& self)
{
    std::vector<std::shared_ptr<OpenXmlPackagePart>> parts;
    std::unordered_set<const OpenXmlPackagePart*> seen;

    const std::function<void(const std::shared_ptr<OpenXmlPackagePart>&)> collect =
        [&](const std::shared_ptr<OpenXmlPackagePart>& part)
    {
        if (part && seen.insert(part.get()).second)
        {
            parts.push_back(part);
        }
    };

    // The registry owns every part the package created or loaded. The graph is
    // walked as well, because a part attached without being registered would
    // otherwise keep its own subtree alive.
    parts.reserve(partsByUri.size());
    for (const auto& [uri, part] : partsByUri)
    {
        collect(part);
    }
    self.ForEachPart(collect);

    const std::function<bool(const OpenXmlPackagePart&)> releaseEverything =
        [](const OpenXmlPackagePart&)
    {
        return true;
    };

    for (const auto& part : parts)
    {
        part->ReleaseChildParts(releaseEverything);
    }
}

bool OpenXmlPackageImpl::LoadFromZip(OpenXmlPackage& self,
                                     zip_t* archive,
                                     const ICancellationToken* cancellationToken)
{
    if (!ValidateZipMetadata(archive, cancellationToken))
    {
        return false;
    }
    if (!ReadContentTypes(archive, cancellationToken))
    {
        return false;
    }
    std::vector<LoadedRelationship> relationships;
    if (!ReadRelationships(archive, relationships, cancellationToken))
    {
        return false;
    }
    retainOriginalBytes = ResolveOriginalByteRetention(relationships);
    if (!ReadParts(self, archive, relationships, cancellationToken))
    {
        return false;
    }
    for (const auto& relationship : relationships)
    {
        if (OpenXmlPackageHelper::IsCancellationRequested(cancellationToken))
        {
            return false;
        }
        if (relationship.relationship.IsExternal)
        {
            if (relationship.sourceUri == "/")
            {
                self.RegisterRelationship(relationship.relationship);
            }
            else if (auto source = self.GetPartByUri(relationship.sourceUri))
            {
                source->RegisterRelationship(relationship.relationship);
            }
            continue;
        }
        const auto absoluteTarget =
            ResolveRelationshipTarget(relationship.sourceUri, relationship.relationship.Target);
        auto child = self.GetPartByUri(absoluteTarget);
        if (!child)
        {
            if (relationship.sourceUri == "/")
            {
                self.RegisterRelationship(relationship.relationship);
            }
            else if (auto source = self.GetPartByUri(relationship.sourceUri))
            {
                source->RegisterRelationship(relationship.relationship);
            }
            continue;
        }
        if (relationship.sourceUri == "/")
        {
            self.AttachExistingChildPart(child,
                                         relationship.relationship.Id,
                                         relationship.relationship.Target,
                                         relationship.relationship.Type,
                                         false,
                                         relationship.relationship.TargetMode);
            continue;
        }
        auto parent = self.GetPartByUri(relationship.sourceUri);
        if (!parent)
        {
            continue;
        }
        parent->AttachExistingChildPart(child,
                                        relationship.relationship.Id,
                                        relationship.relationship.Target,
                                        relationship.relationship.Type,
                                        relationship.relationship.IsExternal,
                                        relationship.relationship.TargetMode);
    }
    return true;
}

void OpenXmlPackageImpl::ReportLimitExceeded(ValidationDomain domain,
                                             ValidationErrorId id,
                                             std::string message,
                                             std::string_view entryName,
                                             std::string_view partUri)
{
    ValidationIssue issue;
    issue.Severity = ValidationSeverity::Error;
    issue.Domain = domain;
    issue.Id = id;
    issue.Message = std::move(message);
    issue.Location.Path = std::string(entryName);
    issue.PartUri = std::string(partUri);
    lastValidationResult.AddIssue(std::move(issue));
}

bool OpenXmlPackageImpl::CheckCurrentEntryLimits(zip_t* archive, std::string_view entryName, bool isXml)
{
    const auto reason = packageLimits.CheckEntry(zip_entry_size(archive), zip_entry_comp_size(archive));
    if (!reason.empty())
    {
        ReportLimitExceeded(ValidationDomain::Opc,
                            ValidationErrorId::OpcLimitExceeded,
                            std::string(reason),
                            entryName,
                            isXml ? Detail::NormalizePartUri(entryName) : std::string{});
        return false;
    }
    return true;
}

bool OpenXmlPackageImpl::ValidateZipMetadata(zip_t* archive, const ICancellationToken* cancellationToken)
{
    const auto total = zip_entries_total(archive);
    if (total < 0)
    {
        ReportLimitExceeded(ValidationDomain::Opc,
                            ValidationErrorId::OpcLimitExceeded,
                            "ZIP entry table cannot be read",
                            {},
                            {});
        return false;
    }
    if (const auto reason = packageLimits.CheckEntryCount(static_cast<UInt64>(total)); !reason.empty())
    {
        ReportLimitExceeded(ValidationDomain::Opc,
                            ValidationErrorId::OpcLimitExceeded,
                            std::string(reason),
                            {},
                            {});
        return false;
    }

    UInt64 compressedTotal = 0;
    UInt64 uncompressedTotal = 0;
    for (Size index = 0; index < static_cast<Size>(total); ++index)
    {
        if (OpenXmlPackageHelper::IsCancellationRequested(cancellationToken))
        {
            return false;
        }
        if (zip_entry_openbyindex(archive, index) != 0)
        {
            continue;
        }
        const char* rawName = zip_entry_name(archive);
        const std::string entryName = rawName ? OpenXmlPackageHelper::NormalizeEntryName(rawName) : std::string{};
        const auto compressed = zip_entry_comp_size(archive);
        const auto uncompressed = zip_entry_size(archive);
        const bool isXml = entryName == OpenXmlPackageHelper::kContentTypesEntry || OpenXmlPackageHelper::EndsWith(entryName, OpenXmlPackageHelper::kRelationshipsExtension) || Detail::PartUriExtension(Detail::NormalizePartUri(entryName)) == "xml";
        if (!CheckCurrentEntryLimits(archive, entryName, isXml))
        {
            zip_entry_close(archive);
            return false;
        }
        zip_entry_close(archive);

        if (compressed > std::numeric_limits<UInt64>::max() - compressedTotal || uncompressed > std::numeric_limits<UInt64>::max() - uncompressedTotal)
        {
            ReportLimitExceeded(ValidationDomain::Opc,
                                ValidationErrorId::OpcLimitExceeded,
                                "ZIP package size accounting overflowed configured counters",
                                entryName,
                                {});
            return false;
        }
        compressedTotal += compressed;
        uncompressedTotal += uncompressed;
        if (const auto reason = packageLimits.CheckTotals(compressedTotal, uncompressedTotal); !reason.empty())
        {
            ReportLimitExceeded(ValidationDomain::Opc,
                                ValidationErrorId::OpcLimitExceeded,
                                std::string(reason),
                                entryName,
                                {});
            return false;
        }
    }
    return true;
}

bool OpenXmlPackageImpl::CheckXmlLimits(const Pugi::xml_document& doc,
                                        std::string_view entryName,
                                        std::string_view partUri,
                                        const ICancellationToken* cancellationToken)
{
    const auto& limits = packageLimits;
    if (OpenXmlPackageHelper::IsCancellationRequested(cancellationToken))
    {
        return false;
    }
    if (limits.MaxXmlDepth == 0 && limits.MaxXmlNodes == 0 && limits.MaxXmlAttributes == 0 && limits.MaxXmlTextCharacters == 0)
    {
        return true;
    }

    UInt64 nodes = 0;
    UInt64 attributes = 0;
    UInt64 textCharacters = 0;

    // The walk is iterative. Recursing per level would mean the check that
    // exists to bound a hostile document could itself be overflowed by one - a
    // caller that sets a node or attribute limit but leaves the depth limit at
    // "no limit" would have no protection at all while walking.
    //
    // It also keeps no queue of nodes still to visit, and follows pugixml's
    // parent and sibling links instead. A queue would have to hold every child
    // of an element before a single one of them is counted, so an element with
    // millions of children would force that allocation even where MaxXmlNodes
    // is a hundred - the input would be rejected only after paying for it.
    const auto visit = [&](const Pugi::xml_node& node, UInt64 depth) -> bool
    {
        if (OpenXmlPackageHelper::IsCancellationRequested(cancellationToken))
        {
            return false;
        }
        if (limits.MaxXmlDepth != 0 && depth > limits.MaxXmlDepth)
        {
            ReportLimitExceeded(ValidationDomain::Xml,
                                ValidationErrorId::XmlLimitExceeded,
                                "XML document exceeds configured depth limit",
                                entryName,
                                partUri);
            return false;
        }
        if (limits.MaxXmlNodes != 0 && ++nodes > limits.MaxXmlNodes)
        {
            ReportLimitExceeded(ValidationDomain::Xml,
                                ValidationErrorId::XmlLimitExceeded,
                                "XML document exceeds configured node count limit",
                                entryName,
                                partUri);
            return false;
        }
        for (auto attr = node.first_attribute(); attr; attr = attr.next_attribute())
        {
            if (limits.MaxXmlAttributes != 0 && ++attributes > limits.MaxXmlAttributes)
            {
                ReportLimitExceeded(ValidationDomain::Xml,
                                    ValidationErrorId::XmlLimitExceeded,
                                    "XML document exceeds configured attribute count limit",
                                    entryName,
                                    partUri);
                return false;
            }
        }
        if ((node.type() == Pugi::node_pcdata || node.type() == Pugi::node_cdata) && limits.MaxXmlTextCharacters != 0)
        {
            textCharacters += std::strlen(node.value());
            if (textCharacters > limits.MaxXmlTextCharacters)
            {
                ReportLimitExceeded(ValidationDomain::Xml,
                                    ValidationErrorId::XmlLimitExceeded,
                                    "XML document exceeds configured text size limit",
                                    entryName,
                                    partUri);
                return false;
            }
        }
        return true;
    };

    Pugi::xml_node current = doc.first_child();
    UInt64 depth = 1;
    while (current)
    {
        if (!visit(current, depth))
        {
            return false;
        }

        if (const auto child = current.first_child())
        {
            current = child;
            ++depth;
            continue;
        }

        // A leaf: climb to the nearest ancestor that still has a sibling left.
        // Reaching depth zero means the document node, and with it the end.
        while (!current.next_sibling())
        {
            current = current.parent();
            if (--depth == 0)
            {
                return true;
            }
        }
        current = current.next_sibling();
    }
    return true;
}

bool OpenXmlPackageImpl::SaveToZip(const OpenXmlPackage& self,
                                   zip_t* archive,
                                   const ICancellationToken* cancellationToken) const
{
    if (!WriteContentTypes(self, archive, cancellationToken))
    {
        return false;
    }
    if (!WriteParts(self, archive, cancellationToken))
    {
        return false;
    }
    if (!WriteRelationships(self, archive, cancellationToken))
    {
        return false;
    }
    return true;
}

bool OpenXmlPackageImpl::ReadContentTypes(zip_t* archive, const ICancellationToken* cancellationToken)
{
    if (OpenXmlPackageHelper::IsCancellationRequested(cancellationToken))
    {
        return false;
    }
    if (zip_entry_open(archive, std::string(OpenXmlPackageHelper::kContentTypesEntry).c_str()) != 0)
    {
        return false;
    }
    if (!CheckCurrentEntryLimits(archive, OpenXmlPackageHelper::kContentTypesEntry, true))
    {
        zip_entry_close(archive);
        return false;
    }
    void* buffer = nullptr;
    Size bufferSize = 0;
    const auto read = zip_entry_read(archive, &buffer, &bufferSize);
    zip_entry_close(archive);
    if (read <= 0 || buffer == nullptr)
    {
        return false;
    }
    Pugi::xml_document doc;
    if (!doc.load_buffer(buffer, bufferSize, Xml::ParseOptions::Preserving))
    {
        std::free(buffer);
        return false;
    }
    if (!CheckXmlLimits(doc, OpenXmlPackageHelper::kContentTypesEntry, {}, cancellationToken))
    {
        std::free(buffer);
        return false;
    }
    std::free(buffer);
    defaultContentTypes.clear();
    contentTypeOverrides.clear();
    const auto types = doc.child("Types");
    for (const auto& child : types.children())
    {
        if (OpenXmlPackageHelper::IsCancellationRequested(cancellationToken))
        {
            return false;
        }
        if (std::strcmp(child.name(), "Default") == 0)
        {
            defaultContentTypes.emplace(child.attribute("Extension").as_string(),
                                        child.attribute("ContentType").as_string());
            continue;
        }
        if (std::strcmp(child.name(), "Override") == 0)
        {
            std::string partName = Detail::NormalizePartUri(child.attribute("PartName").as_string());
            contentTypeOverrides.emplace(std::move(partName), child.attribute("ContentType").as_string());
        }
    }
    return true;
}

bool OpenXmlPackageImpl::WriteContentTypes(const OpenXmlPackage& /*self*/,
                                           zip_t* archive,
                                           const ICancellationToken* cancellationToken) const
{
    if (OpenXmlPackageHelper::IsCancellationRequested(cancellationToken))
    {
        return false;
    }
    auto defaults = defaultContentTypes;
    auto overrides = contentTypeOverrides;

    defaults.try_emplace("rels", std::string(OpenXmlPackageHelper::kRelationshipsContentType));
    defaults.try_emplace("xml", "application/xml");

    for (const auto& [uri, part] : partsByUri)
    {
        if (OpenXmlPackageHelper::IsCancellationRequested(cancellationToken))
        {
            return false;
        }
        const std::string contentType(part->ContentType());
        if (auto overrideIt = overrides.find(uri); overrideIt != overrides.end())
        {
            overrideIt->second = contentType;
            continue;
        }

        const auto extension = Detail::PartUriExtension(uri);
        if (auto defaultIt = defaults.find(extension);
            defaultIt != defaults.end() && defaultIt->second == contentType)
        {
            continue;
        }

        overrides[uri] = contentType;
    }

    Pugi::xml_document doc;
    auto types = doc.append_child("Types");
    types.append_attribute("xmlns") = "http://schemas.openxmlformats.org/package/2006/content-types";
    for (const auto& [extension, contentType] : defaults)
    {
        if (OpenXmlPackageHelper::IsCancellationRequested(cancellationToken))
        {
            return false;
        }
        auto defaultNode = types.append_child("Default");
        defaultNode.append_attribute("Extension") = extension.c_str();
        defaultNode.append_attribute("ContentType") = contentType.c_str();
    }
    for (const auto& [uri, contentType] : overrides)
    {
        if (OpenXmlPackageHelper::IsCancellationRequested(cancellationToken))
        {
            return false;
        }
        auto overrideNode = types.append_child("Override");
        overrideNode.append_attribute("PartName") = uri.c_str();
        overrideNode.append_attribute("ContentType") = contentType.c_str();
    }
    std::ostringstream stream;
    doc.save(stream);
    const auto xml = stream.str();
    if (zip_entry_open(archive, std::string(OpenXmlPackageHelper::kContentTypesEntry).c_str()) != 0)
    {
        return false;
    }
    const auto writeResult = zip_entry_write(archive, xml.data(), xml.size());
    zip_entry_close(archive);
    return writeResult == 0;
}

bool OpenXmlPackageImpl::ReadRelationships(zip_t* archive,
                                           std::vector<LoadedRelationship>& output,
                                           const ICancellationToken* cancellationToken)
{
    const auto total = zip_entries_total(archive);
    for (Size index = 0; index < static_cast<Size>(total); ++index)
    {
        if (OpenXmlPackageHelper::IsCancellationRequested(cancellationToken))
        {
            return false;
        }
        if (zip_entry_openbyindex(archive, index) != 0)
        {
            continue;
        }
        const char* rawName = zip_entry_name(archive);
        if (!rawName)
        {
            zip_entry_close(archive);
            continue;
        }
        const std::string entryName = OpenXmlPackageHelper::NormalizeEntryName(rawName);
        if (!OpenXmlPackageHelper::EndsWith(entryName, OpenXmlPackageHelper::kRelationshipsExtension))
        {
            zip_entry_close(archive);
            continue;
        }
        if (entryName == OpenXmlPackageHelper::kContentTypesEntry)
        {
            zip_entry_close(archive);
            continue;
        }
        if (!CheckCurrentEntryLimits(archive, entryName, true))
        {
            zip_entry_close(archive);
            return false;
        }
        void* buffer = nullptr;
        Size bufferSize = 0;
        const auto read = zip_entry_read(archive, &buffer, &bufferSize);
        zip_entry_close(archive);
        if (read <= 0 || buffer == nullptr)
        {
            continue;
        }
        Pugi::xml_document doc;
        if (!doc.load_buffer(buffer, bufferSize, Xml::ParseOptions::Preserving))
        {
            std::free(buffer);
            continue;
        }
        if (!CheckXmlLimits(doc, entryName, {}, cancellationToken))
        {
            std::free(buffer);
            return false;
        }
        std::free(buffer);
        std::string containerUri;
        if (entryName == "_rels/.rels")
        {
            containerUri = "/";
        }
        else
        {
            auto slash = entryName.rfind("/_rels/");
            if (slash == std::string::npos)
            {
                containerUri.clear();
            }
            else
            {
                auto folder = entryName.substr(0, slash);
                auto file = entryName.substr(slash + 7);
                if (OpenXmlPackageHelper::EndsWith(file, ".rels"))
                {
                    file.resize(file.size() - 5);
                }
                containerUri = Detail::NormalizePartUri(folder + "/" + file);
            }
        }
        // Counted per relationships part, which is what MaxRelationships is
        // documented to bound. Counting the package-wide total instead would
        // reject a package built from many small, individually legitimate
        // .rels parts.
        UInt64 relationshipsInPart = 0;
        for (const auto& relNode : doc.child("Relationships").children("Relationship"))
        {
            if (OpenXmlPackageHelper::IsCancellationRequested(cancellationToken))
            {
                return false;
            }
            ++relationshipsInPart;
            if (packageLimits.MaxRelationships != 0 && relationshipsInPart > packageLimits.MaxRelationships)
            {
                ReportLimitExceeded(ValidationDomain::Opc,
                                    ValidationErrorId::OpcLimitExceeded,
                                    "Package exceeds configured relationship count limit",
                                    entryName,
                                    containerUri);
                return false;
            }
            OpenXmlRelationship relationship;
            relationship.Id = relNode.attribute("Id").as_string();
            relationship.Type = relNode.attribute("Type").as_string();
            relationship.Target = relNode.attribute("Target").as_string();
            relationship.TargetMode = relNode.attribute("TargetMode").as_string();
            relationship.IsExternal = relationship.TargetMode == "External";
            output.push_back(LoadedRelationship{containerUri, std::move(relationship)});
        }
    }
    return true;
}

bool OpenXmlPackageImpl::WriteRelationships(const OpenXmlPackage& self,
                                            zip_t* archive,
                                            const ICancellationToken* cancellationToken) const
{
    auto writeContainer = [&](const OpenXmlPartContainer& container, std::string relPath) -> bool
    {
        if (OpenXmlPackageHelper::IsCancellationRequested(cancellationToken))
        {
            return false;
        }
        if (container.Relationships().empty())
        {
            return true;
        }
        Pugi::xml_document doc;
        auto relationships = doc.append_child("Relationships");
        relationships.append_attribute("xmlns") = "http://schemas.openxmlformats.org/package/2006/relationships";
        for (const auto& relationship : container.Relationships())
        {
            if (OpenXmlPackageHelper::IsCancellationRequested(cancellationToken))
            {
                return false;
            }
            auto relNode = relationships.append_child("Relationship");
            relNode.append_attribute("Id") = relationship.Id.c_str();
            relNode.append_attribute("Type") = relationship.Type.c_str();
            relNode.append_attribute("Target") = relationship.Target.c_str();
            if (relationship.IsExternal)
            {
                relNode.append_attribute("TargetMode") = "External";
            }
        }
        std::ostringstream stream;
        doc.save(stream);
        const auto xml = stream.str();
        if (zip_entry_open(archive, relPath.c_str()) != 0)
        {
            return false;
        }
        const auto result = zip_entry_write(archive, xml.data(), xml.size());
        zip_entry_close(archive);
        return result == 0;
    };
    if (!writeContainer(self, "_rels/.rels"))
    {
        return false;
    }
    for (const auto& [uri, part] : partsByUri)
    {
        if (OpenXmlPackageHelper::IsCancellationRequested(cancellationToken))
        {
            return false;
        }
        if (part->Relationships().empty())
        {
            continue;
        }
        const auto entryPath = Detail::RelationshipPartEntryName(uri);
        if (!writeContainer(*part, entryPath))
        {
            return false;
        }
    }
    return true;
}

bool OpenXmlPackageImpl::ResolveOriginalByteRetention(const std::vector<LoadedRelationship>& relationships) const
{
    switch (partByteRetention)
    {
        case PartByteRetention::Never:
            return false;
        case PartByteRetention::Always:
            return true;
        case PartByteRetention::WhenSignaturesPresent:
            break;
    }

    for (const auto& [uri, contentType] : contentTypeOverrides)
    {
        if (contentType == Security::SignatureNames::SignatureContentType ||
            contentType == Security::SignatureNames::OriginContentType)
        {
            return true;
        }
    }
    for (const auto& relationship : relationships)
    {
        if (relationship.relationship.Type == Security::SignatureNames::OriginRelationshipType ||
            relationship.relationship.Type == Security::SignatureNames::SignatureRelationshipType)
        {
            return true;
        }
    }
    return false;
}

bool OpenXmlPackageImpl::ReadParts(OpenXmlPackage& self,
                                   zip_t* archive,
                                   const std::vector<LoadedRelationship>& relationships,
                                   const ICancellationToken* cancellationToken)
{
    std::unordered_map<std::string, std::string> relationshipTypes;
    relationshipTypes.reserve(relationships.size());
    for (const auto& relationship : relationships)
    {
        if (OpenXmlPackageHelper::IsCancellationRequested(cancellationToken))
        {
            return false;
        }
        if (relationship.relationship.IsExternal)
        {
            continue;
        }
        const auto absolute =
            ResolveRelationshipTarget(relationship.sourceUri, relationship.relationship.Target);
        relationshipTypes.emplace(absolute, relationship.relationship.Type);
    }
    const auto total = zip_entries_total(archive);
    for (Size index = 0; index < static_cast<Size>(total); ++index)
    {
        if (OpenXmlPackageHelper::IsCancellationRequested(cancellationToken))
        {
            return false;
        }
        if (zip_entry_openbyindex(archive, index) != 0)
        {
            continue;
        }
        const char* rawName = zip_entry_name(archive);
        if (!rawName)
        {
            zip_entry_close(archive);
            continue;
        }
        const std::string entryName = OpenXmlPackageHelper::NormalizeEntryName(rawName);
        if (entryName == OpenXmlPackageHelper::kContentTypesEntry || OpenXmlPackageHelper::EndsWith(entryName, OpenXmlPackageHelper::kRelationshipsExtension))
        {
            zip_entry_close(archive);
            continue;
        }
        const auto partUri = Detail::NormalizePartUri(entryName);
        const bool isXmlEntry = Detail::PartUriExtension(partUri) == "xml";
        if (!CheckCurrentEntryLimits(archive, entryName, isXmlEntry))
        {
            zip_entry_close(archive);
            return false;
        }
        void* bufferPtr = nullptr;
        Size bufferSize = 0;
        if (zip_entry_read(archive, &bufferPtr, &bufferSize) < 0 || bufferPtr == nullptr)
        {
            zip_entry_close(archive);
            continue;
        }
        zip_entry_close(archive);
        const auto* bytes = static_cast<const UInt8*>(bufferPtr);
        std::vector<Byte> buffer(bytes, bytes + bufferSize);
        std::free(bufferPtr);
        if (self.GetPartByUri(partUri) != nullptr)
        {
            duplicatePartUris.push_back(partUri);
            continue;
        }
        std::string relationshipType;
        if (auto rel = relationshipTypes.find(partUri); rel != relationshipTypes.end())
        {
            relationshipType = rel->second;
        }
        std::string contentType;
        if (auto it = contentTypeOverrides.find(partUri); it != contentTypeOverrides.end())
        {
            contentType = it->second;
        }
        else
        {
            const auto extension = Detail::PartUriExtension(partUri);
            if (auto def = defaultContentTypes.find(extension); def != defaultContentTypes.end())
            {
                contentType = def->second;
            }
            else
            {
                contentType = "application/octet-stream";
            }
        }
        auto part = Generated::CreatePackagePart(relationshipType, contentType, self);
        if (!part)
        {
            const bool isXml = contentType == "application/xml" || contentType.find("+xml") != std::string::npos || Detail::PartUriExtension(partUri) == "xml";
            part = std::make_shared<OpaquePackagePart>(relationshipType,
                                                       contentType,
                                                       isXml ? OpenXmlPartKind::Xml : OpenXmlPartKind::Binary,
                                                       &self);
        }
        part->SetPackage(&self);
        part->SetUri(partUri);
        part->SetContentType(contentType);
        self.RegisterPart(part);
        if (part->IsXmlPart())
        {
            if (OpenXmlPackageHelper::IsCancellationRequested(cancellationToken))
            {
                return false;
            }
            if (retainOriginalBytes)
            {
                part->SetOriginalBytes(buffer);
            }
            std::string xml(buffer.begin(), buffer.end());
            Pugi::xml_document doc;
            if (!doc.load_buffer(xml.data(), xml.size(), Xml::ParseOptions::Preserving))
            {
                return false;
            }
            if (!CheckXmlLimits(doc, entryName, partUri, cancellationToken))
            {
                return false;
            }
            part->SetXmlString(xml);
        }
        else
        {
            if (OpenXmlPackageHelper::IsCancellationRequested(cancellationToken))
            {
                return false;
            }
            part->SetBinaryData(std::move(buffer));
        }
    }
    return true;
}

bool OpenXmlPackageImpl::WriteParts(const OpenXmlPackage& /*self*/,
                                    zip_t* archive,
                                    const ICancellationToken* cancellationToken) const
{
    for (const auto& [uri, part] : partsByUri)
    {
        if (OpenXmlPackageHelper::IsCancellationRequested(cancellationToken))
        {
            return false;
        }
        const auto entryName = OpenXmlPackageHelper::TrimLeadingSlash(uri);
        if (zip_entry_open(archive, std::string(entryName).c_str()) != 0)
        {
            return false;
        }
        int result = 0;
        if (part->IsXmlPart())
        {
            const auto xml = part->GetXmlString();
            result = zip_entry_write(archive, xml.data(), xml.size());
        }
        else
        {
            const auto data = part->GetBinaryData();
            if (!data.empty())
            {
                result = zip_entry_write(archive, data.data(), data.size());
            }
        }
        zip_entry_close(archive);
        if (result != 0)
        {
            return false;
        }
    }
    return true;
}

std::string OpenXmlPackageImpl::BuildUniquePartUri(const OpenXmlPackage& self,
                                                   const OpenXmlPartDescriptor& descriptor,
                                                   const OpenXmlPartContainer& container,
                                                   bool allowMultiple,
                                                   std::string_view contentType) const
{
    const auto folder =
        Detail::CombineChildPartFolder(container.ContainerUri(), descriptor.PathFor(self.DocumentFamily()));
    const std::string targetBase(descriptor.Target);
    std::string extension(descriptor.TargetExtension);
    if (extension.empty())
    {
        extension = descriptor.Kind == OpenXmlPartKind::Binary ? ".bin" : ".xml";
    }
    // `.bin` is the placeholder for a payload whose type the descriptor cannot
    // know - an image part holds a PNG, a JPEG or a BMP. Once the part carries a
    // content type, name the file the way Office names it.
    if (extension == ".bin")
    {
        const auto effectiveContentType = contentType.empty() ? descriptor.ContentType : contentType;
        if (auto byContentType = Detail::PartExtensionForContentType(effectiveContentType); !byContentType.empty())
        {
            extension = byContentType;
        }
    }
    auto BuildCandidate = [&](std::optional<UInt32> index) -> std::string
    {
        std::ostringstream builder;
        builder << folder;
        if (!folder.empty() && folder.back() != '/')
        {
            builder << '/';
        }
        builder << targetBase;
        if (index)
        {
            builder << *index;
        }
        builder << extension;
        return builder.str();
    };
    if (!allowMultiple)
    {
        // A part that may occur only once per parent is not necessarily unique
        // in the package: every worksheet owns at most one drawing, yet a
        // workbook may hold many. Keep the plain, unsuffixed name when it is
        // free, but fall back to indexed names instead of silently handing out
        // a URI that another part already occupies.
        auto candidate = BuildCandidate(std::nullopt);
        if (!self.IsPartUriInUse(candidate))
        {
            return candidate;
        }
    }
    UInt32 index = 1;
    while (true)
    {
        auto candidate = BuildCandidate(index);
        if (!self.IsPartUriInUse(candidate))
        {
            return candidate;
        }
        ++index;
    }
}

std::string OpenXmlPackageImpl::NormalizePartUriFilePath(const std::filesystem::path& uri) const
{
    return Detail::NormalizePartUri(uri.generic_string());
}

std::string OpenXmlPackageImpl::NormalizePartUriString(const std::string& uri) const
{
    return Detail::NormalizePartUri(uri);
}

std::string OpenXmlPackageImpl::NormalizePartUriStringView(std::string_view uri) const
{
    return Detail::NormalizePartUri(uri);
}

std::string OpenXmlPackageImpl::ResolveRelationshipTarget(std::string_view sourceUri,
                                                          std::string_view targetUri) const
{
    return Detail::ResolveRelationshipTarget(sourceUri, targetUri);
}

std::string OpenXmlPackageImpl::BuildRelationshipTarget(const std::string& sourceUri,
                                                        const std::string& targetUri) const
{
    return Detail::BuildRelationshipTarget(sourceUri, targetUri);
}

bool OpenXmlPartContainer::AttachExistingChildPart(const std::shared_ptr<OpenXmlPackagePart>& part,
                                                   std::string relationshipId,
                                                   std::string target,
                                                   std::string relationshipType,
                                                   bool isExternal,
                                                   std::string targetMode)
{
    if (!part)
    {
        return false;
    }
    if (!HasPackage())
    {
        return false;
    }
    part->SetPackage(Package());
    m_impl->childParts.push_back(part);
    OpenXmlRelationship relationship;
    relationship.Id = std::move(relationshipId);
    relationship.Type = std::move(relationshipType);
    relationship.Target = std::move(target);
    relationship.TargetMode = std::move(targetMode);
    relationship.IsExternal = isExternal;
    if (!relationship.IsExternal)
    {
        part->AddIncomingRelationship(this, relationship);
    }
    RegisterRelationship(std::move(relationship));

    return true;
}
} // namespace ExyokiOffice
