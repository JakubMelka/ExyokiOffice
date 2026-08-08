// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/OpenXmlPackagePart.hpp"
#include "ExyokiOffice/ValidationResult.hpp"

#include "pugixml/pugixml.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct zip_t;

namespace ExyokiOffice
{

class OpenXmlPackage;
class OpenXmlPackagePart;
class OpenXmlPartContainer;
class ICancellationToken;
struct OpenXmlRelationship;
struct OpenXmlPartDescriptor;

struct XmlDocumentStorage
{
public:
    ExyokiOffice::Pugi::xml_document Document;
};

struct LoadedRelationship
{
    std::string sourceUri;
    OpenXmlRelationship relationship;
};

struct OpenXmlIncomingRelationshipState
{
    OpenXmlPartContainer* source = nullptr;
    OpenXmlIncomingRelationship relationship;
};

struct OpenXmlPartContainerImpl
{
    OpenXmlPackage* package = nullptr;
    std::vector<std::shared_ptr<OpenXmlPackagePart>> childParts;
    std::vector<OpenXmlRelationship> relationships;
    UInt32 nextRelationshipId = 1;
};

struct OpenXmlPackagePartImpl
{
    /// A descriptor holds non-owning views, but a part outlives the descriptor
    /// it was created from, so every string is copied here and the stored
    /// descriptor is rebuilt to point at those copies.
    explicit OpenXmlPackagePartImpl(const OpenXmlPartDescriptor& source)
        : descriptorName(source.Name), descriptorRelationshipType(source.RelationshipType), descriptorContentType(source.ContentType), descriptorTarget(source.Target), descriptorTargetExtension(source.TargetExtension), descriptorDefaultPath(source.DefaultPath), descriptorWordPath(source.WordPath), descriptorExcelPath(source.ExcelPath), descriptorPowerPointPath(source.PowerPointPath)
    {
        descriptor.Name = descriptorName;
        descriptor.RelationshipType = descriptorRelationshipType;
        descriptor.ContentType = descriptorContentType;
        descriptor.Target = descriptorTarget;
        descriptor.TargetExtension = descriptorTargetExtension;
        descriptor.DefaultPath = descriptorDefaultPath;
        descriptor.Kind = source.Kind;
        descriptor.Version = source.Version;
        descriptor.WordPath = descriptorWordPath;
        descriptor.ExcelPath = descriptorExcelPath;
        descriptor.PowerPointPath = descriptorPowerPointPath;
    }

    std::string descriptorName;
    std::string descriptorRelationshipType;
    std::string descriptorContentType;
    std::string descriptorTarget;
    std::string descriptorTargetExtension;
    std::string descriptorDefaultPath;
    std::string descriptorWordPath;
    std::string descriptorExcelPath;
    std::string descriptorPowerPointPath;
    OpenXmlPartDescriptor descriptor;
    std::string uri;
    std::string relationshipId;
    OpenXmlPartContainer* parent = nullptr;
    std::vector<OpenXmlIncomingRelationshipState> incomingRelationships;
    mutable std::vector<OpenXmlIncomingRelationship> incomingRelationshipView;
    std::unique_ptr<XmlDocumentStorage> xmlData;
    /// The characters an XML part was last given, kept only for the parts a
    /// digital signature covers byte for byte. See RequiresByteStableXml.
    std::string verbatimXml;
    bool hasVerbatimXml = false;
    std::vector<Byte> binaryData;
    std::string contentTypeOverride;
    /// Bytes as stored in the loaded package; kept only when the package asks
    /// for it, because a digital signature digests the stored stream and saving
    /// re-serializes XML parts.
    std::vector<Byte> originalBytes;
    bool hasOriginalBytes = false;
};

struct OpenXmlPackageImpl
{
    std::filesystem::path packagePath;
    std::unordered_map<std::string, std::shared_ptr<OpenXmlPackagePart>> partsByUri;
    std::unordered_map<std::string, std::string> contentTypeOverrides;
    std::unordered_map<std::string, std::string> defaultContentTypes;
    std::vector<std::string> duplicatePartUris;
    ValidationResult lastValidationResult;
    OpenXmlPackageLimits packageLimits;
    PartByteRetention partByteRetention = PartByteRetention::WhenSignaturesPresent;
    SignatureSavePolicy signatureSavePolicy = SignatureSavePolicy::Warn;
    /// Resolved from partByteRetention once the loader knows whether the package
    /// carries digital signature parts.
    bool retainOriginalBytes = false;
    /// One-shot: set by signing, cleared by the save that honours it, so a signed
    /// package is written with the very bytes its digests were computed over.
    bool suppressSaveTimePropertyUpdate = false;
    /// Caller supplied access to resources outside the package. Null by default,
    /// which is what makes the whole external resource path inert.
    Security::IExternalResourceResolver::Ptr externalResourceResolver;
    /// Deny-everything until the caller widens it.
    Security::ExternalResourcePolicy externalResourcePolicy;
    /// Budget consumed since the package was loaded or the budget was reset.
    UInt32 externalRequestCount = 0;
    UInt64 externalBytesUsed = 0;

    bool LoadFromZip(OpenXmlPackage& self, zip_t* archive, const ICancellationToken* cancellationToken);
    bool SaveToZip(const OpenXmlPackage& self, zip_t* archive, const ICancellationToken* cancellationToken) const;
    bool ValidateZipMetadata(zip_t* archive, const ICancellationToken* cancellationToken);
    bool CheckCurrentEntryLimits(zip_t* archive, std::string_view entryName, bool isXml);
    bool CheckXmlLimits(const Pugi::xml_document& doc,
                        std::string_view entryName,
                        std::string_view partUri,
                        const ICancellationToken* cancellationToken);
    void ReportLimitExceeded(ValidationDomain domain,
                             ValidationErrorId id,
                             std::string message,
                             std::string_view entryName,
                             std::string_view partUri);

    bool ReadContentTypes(zip_t* archive, const ICancellationToken* cancellationToken);
    bool WriteContentTypes(const OpenXmlPackage& self,
                           zip_t* archive,
                           const ICancellationToken* cancellationToken) const;

    bool ReadRelationships(zip_t* archive,
                           std::vector<LoadedRelationship>& output,
                           const ICancellationToken* cancellationToken);

    /// Decides whether the loader keeps the bytes it reads, based on the
    /// configured policy and on whether the package carries signature parts.
    bool ResolveOriginalByteRetention(const std::vector<LoadedRelationship>& relationships) const;
    bool WriteRelationships(const OpenXmlPackage& self,
                            zip_t* archive,
                            const ICancellationToken* cancellationToken) const;

    bool ReadParts(OpenXmlPackage& self,
                   zip_t* archive,
                   const std::vector<LoadedRelationship>& relationships,
                   const ICancellationToken* cancellationToken);
    bool WriteParts(const OpenXmlPackage& self, zip_t* archive, const ICancellationToken* cancellationToken) const;
    void RemoveUnreachableParts(const OpenXmlPackage& self);
    /// Cuts every owning edge between the parts of the package, so that the
    /// cycles the relationship graph legitimately contains cannot keep the parts
    /// alive once the package lets go of them. Only called while the package is
    /// discarding its whole content.
    void ReleaseAllPartOwnership(const OpenXmlPackage& self);

    std::string BuildUniquePartUri(const OpenXmlPackage& self,
                                   const OpenXmlPartDescriptor& descriptor,
                                   const OpenXmlPartContainer& container,
                                   bool allowMultiple,
                                   std::string_view contentType) const;
    std::string NormalizePartUriFilePath(const std::filesystem::path& uri) const;
    std::string NormalizePartUriString(const std::string& uri) const;
    std::string NormalizePartUriStringView(std::string_view uri) const;
    std::string ResolveRelationshipTarget(std::string_view sourceUri,
                                          std::string_view targetUri) const;
    std::string BuildRelationshipTarget(const std::string& sourceUri,
                                        const std::string& targetUri) const;
};

} // namespace ExyokiOffice
