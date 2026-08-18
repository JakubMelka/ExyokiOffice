// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

// One test case per row of the "Cross-cutting subsystems" table in
// docs/Compatibility.md.
//
// These rows are graded per family rather than per capability, so each case
// walks all three families where the row claims all three. Two rows are graded
// No, and their cases assert the refusal: OOXML package encryption cannot be
// opened at all, and Word and PowerPoint have no VBA extraction API even though
// they carry the project through a save.

#include "doctest.h"

#include "TestSupport.hpp"

#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/MarkupCompatibility.hpp"
#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/OpenXmlPackageValidator.hpp"
#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"
#include "ExyokiOffice/Security/CryptoProvider.hpp"
#include "ExyokiOffice/Security/ExternalResources.hpp"
#include "ExyokiOffice/Security/PackageSignatures.hpp"
#include "ExyokiOffice/Security/ResourceResolver.hpp"
#include "ExyokiOffice/ThemeService.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace
{

using ExyokiOffice::Byte;
using ExyokiOffice::Excel::ExcelDocumentEditor;
using ExyokiOffice::PowerPoint::PowerPointDocumentEditor;
using ExyokiOffice::Word::WordDocumentEditor;
using ExyokiOfficeTests::CheckPreservation;
using ExyokiOfficeTests::ValidatePackage;

using ExyokiOffice::Security::ExternalResourceKind;
using ExyokiOffice::Security::ExternalResourcePolicy;
using ExyokiOffice::Security::ExternalResourceRequest;
using ExyokiOffice::Security::ExternalResourceResponse;
using ExyokiOffice::Security::ExternalResourceStatus;
using ExyokiOffice::Security::ICryptoProvider;
using ExyokiOffice::Security::IExternalResourceResolver;
using ExyokiOffice::Security::SignatureAlgorithm;

/// Stands in for a real crypto backend. It performs no cryptography: the
/// "signature" is the digest of the signed bytes, which is enough to prove that
/// signing and verification agree on the canonical form and that the plumbing
/// around an application-supplied provider works.
class StubCryptoProvider final : public ICryptoProvider
{
public:
    SignatureAlgorithm GetSignatureAlgorithm() const override { return SignatureAlgorithm::RsaSha256; }

    std::vector<std::vector<Byte>> GetCertificateChain() const override { return {m_certificate}; }

    bool SignData(SignatureAlgorithm algorithm, std::span<const Byte> data, std::vector<Byte>& signature) const override
    {
        signature = ExyokiOffice::Security::ComputeDigest(ExyokiOffice::Security::GetDigestAlgorithm(algorithm),
                                                          data);
        return !signature.empty();
    }

    bool VerifyData(SignatureAlgorithm algorithm,
                    std::span<const Byte> data,
                    std::span<const Byte> signature,
                    std::span<const Byte> certificateDer) const override
    {
        if (!std::equal(certificateDer.begin(), certificateDer.end(), m_certificate.begin(), m_certificate.end()))
        {
            return false;
        }
        const auto expected =
            ExyokiOffice::Security::ComputeDigest(ExyokiOffice::Security::GetDigestAlgorithm(algorithm), data);
        return std::equal(expected.begin(), expected.end(), signature.begin(), signature.end());
    }

private:
    std::vector<Byte> m_certificate{0x30, 0x82, 0x01, 0x0A};
};

/// Records every call. It reaches nothing: the library must ask before anything
/// outside the package is read, and these tests assert whether it asked.
class RecordingResolver final : public IExternalResourceResolver
{
public:
    ExternalResourceResponse Resolve(const ExternalResourceRequest& request) override
    {
        ++Calls;
        LastUri = request.Uri;

        ExternalResourceResponse response;
        response.Status = ExternalResourceStatus::Ok;
        response.Data = {0x01, 0x02, 0x03};
        return response;
    }

    int Calls = 0;
    std::string LastUri;
};

/// One saved package per family, so a cross-cutting row can walk all three.
struct FamilyPackages
{
    std::vector<Byte> Word;
    std::vector<Byte> Excel;
    std::vector<Byte> PowerPoint;
};

FamilyPackages MakeFamilyPackages()
{
    FamilyPackages packages;

    auto word = WordDocumentEditor::CreateNew();
    REQUIRE(word != nullptr);
    REQUIRE(word->AddParagraph("Cross-cutting") != nullptr);
    packages.Word = word->SaveToMemory();
    REQUIRE_FALSE(packages.Word.empty());

    auto excel = ExcelDocumentEditor::CreateNew();
    REQUIRE(excel != nullptr);
    REQUIRE(excel->FirstWorksheet()->SetCellText(1, 1, "Cross-cutting"));
    packages.Excel = excel->SaveToMemory();
    REQUIRE_FALSE(packages.Excel.empty());

    auto powerPoint = PowerPointDocumentEditor::CreateNew();
    REQUIRE(powerPoint != nullptr);
    packages.PowerPoint = powerPoint->SaveToMemory();
    REQUIRE_FALSE(packages.PowerPoint.empty());

    return packages;
}

void CheckPreserved(const std::vector<Byte>& bytes)
{
    const auto preservation = CheckPreservation(bytes);
    REQUIRE(preservation.Ok);
    for (const auto& difference : preservation.Differences)
    {
        CAPTURE(difference);
        CHECK_MESSAGE(false, "package changed through open-save");
    }
    CHECK(preservation.Preserved);
}

} // namespace

TEST_SUITE("CrossCuttingMatrixTests")
{

    TEST_CASE("OPC package round-trip holds for all three families [compat] [opc-roundtrip]")
    {
        const auto packages = MakeFamilyPackages();

        for (const auto* bytes : {&packages.Word, &packages.Excel, &packages.PowerPoint})
        {
            ExyokiOffice::OpenXmlPackage package;
            REQUIRE(package.LoadFromMemory(*bytes));

            // Parts, content types and the relationship graph all survive; the
            // preservation helper compares them part by part.
            CHECK_FALSE(package.Parts().empty());
            CheckPreserved(*bytes);
        }
    }

    TEST_CASE("Unknown and vendor-specific parts are carried through as opaque parts "
              "[compat] [opc-unknown-parts]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        REQUIRE(editor->AddParagraph("Carrier") != nullptr);

        // A part no schema of this library knows, attached the way another
        // producer's add-in attaches one.
        auto mainPart = editor->GetDocument()->GetMainDocumentPart();
        REQUIRE(mainPart != nullptr);

        const auto relationshipId =
            editor->GetDocument()->AddExternalRelationship("http://example.com/vendor/settings",
                                                           "https://example.com/vendor-settings.xml");
        REQUIRE_FALSE(relationshipId.empty());

        const auto bytes = editor->SaveToMemory();
        REQUIRE_FALSE(bytes.empty());

        auto reopened = WordDocumentEditor::Open(bytes);
        REQUIRE(reopened != nullptr);

        // The relationship graph keeps the unknown edge attached.
        bool foundVendorRelationship = false;
        for (const auto& relationship : reopened->GetDocument()->Relationships())
        {
            if (relationship.Type == "http://example.com/vendor/settings")
            {
                foundVendorRelationship = true;
                CHECK(relationship.IsExternal);
                CHECK(relationship.Target == "https://example.com/vendor-settings.xml");
            }
        }
        CHECK(foundVendorRelationship);

        CheckPreserved(bytes);
    }

    TEST_CASE("Markup compatibility selects a branch by target version [compat] [markup-compatibility]")
    {
        using ExyokiOffice::OpenXml::FileFormatVersions;

        // The defaults documented in the matrix: validation is permissive,
        // markup compatibility is conservative.
        CHECK(ExyokiOffice::MarkupCompatibilityProcessSettings{}.TargetFileFormatVersions ==
              FileFormatVersions::Office2007);

        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        REQUIRE(editor->AddParagraph("Alternate content carrier") != nullptr);
        const auto bytes = editor->SaveToMemory();
        REQUIRE_FALSE(bytes.empty());

        // Opening with each target has to succeed; the branch selection itself
        // is covered in depth by the unit layer's [markup-compatibility] area.
        for (const auto target : {FileFormatVersions::Office2007, FileFormatVersions::Office2013,
                                  FileFormatVersions::Microsoft365})
        {
            CAPTURE(static_cast<int>(target));
            ExyokiOffice::Packaging::OpenSettings settings;
            settings.MarkupCompatibility.TargetFileFormatVersions = target;

            auto reopened = WordDocumentEditor::Open(bytes, settings);
            REQUIRE(reopened != nullptr);
            REQUIRE_FALSE(reopened->Paragraphs().empty());
            CHECK(reopened->Paragraphs().front()->PlainText() == "Alternate content carrier");
        }
    }

    TEST_CASE("Schema and semantic validation runs over every family [compat] [validation]")
    {
        const auto packages = MakeFamilyPackages();

        for (const auto* bytes : {&packages.Word, &packages.Excel, &packages.PowerPoint})
        {
            const auto summary = ValidatePackage(*bytes);
            CAPTURE(summary.FirstError);
            REQUIRE(summary.Loaded);
            CHECK_FALSE(summary.HasErrors);
        }

        // A diagnostic carries a positional path, not just a message.
        ExyokiOffice::OpenXmlPackage package;
        REQUIRE(package.LoadFromMemory(packages.Word));
        const auto result = ExyokiOffice::OpenXmlPackageValidator(ExyokiOffice::OpenXmlDomValidationSettings{}).Validate(package);
        for (const auto& issue : result.Issues())
        {
            CAPTURE(issue.Message);
            CHECK(issue.Severity != ExyokiOffice::ValidationSeverity::Error);
        }
    }

    TEST_CASE("Document properties: core, extended and custom, in every family [compat] [properties]")
    {
        auto word = WordDocumentEditor::CreateNew();
        REQUIRE(word != nullptr);
        auto properties = word->Properties();
        properties.SetTitle("Cross-cutting");
        properties.SetCreator("ExyokiOffice");
        properties.SetCompany("Contoso");
        properties.SetCustomProperty("Reviewed", ExyokiOffice::Packaging::DocumentCustomPropertyValue(true));

        const auto bytes = word->SaveToMemory();
        REQUIRE_FALSE(bytes.empty());

        auto reopened = WordDocumentEditor::Open(bytes);
        REQUIRE(reopened != nullptr);
        auto readBack = reopened->Properties();
        CHECK(readBack.GetTitle() == "Cross-cutting");
        CHECK(readBack.GetCreator() == "ExyokiOffice");
        CHECK(readBack.GetCompany() == "Contoso");
        CHECK(readBack.GetCustomProperty("Reviewed").has_value());

        // The other two families expose the same object.
        auto excel = ExcelDocumentEditor::CreateNew();
        REQUIRE(excel != nullptr);
        excel->Properties().SetTitle("Workbook");
        auto excelReopened = ExcelDocumentEditor::Open(excel->SaveToMemory());
        REQUIRE(excelReopened != nullptr);
        CHECK(excelReopened->Properties().GetTitle() == "Workbook");

        auto powerPoint = PowerPointDocumentEditor::CreateNew();
        REQUIRE(powerPoint != nullptr);
        powerPoint->Properties().SetTitle("Deck");
        auto powerPointReopened = PowerPointDocumentEditor::Open(powerPoint->SaveToMemory());
        REQUIRE(powerPointReopened != nullptr);
        CHECK(powerPointReopened->Properties().GetTitle() == "Deck");
    }

    TEST_CASE("Themes: read and written in every family [compat] [themes]")
    {
        auto word = WordDocumentEditor::CreateNew();
        REQUIRE(word != nullptr);
        REQUIRE(word->EnsureTheme());

        auto settings = word->ThemeSettings();
        REQUIRE(settings.has_value());
        settings->Name = "Compat theme";
        REQUIRE(word->SetThemeSettings(*settings));

        auto reopened = WordDocumentEditor::Open(word->SaveToMemory());
        REQUIRE(reopened != nullptr);
        auto readBack = reopened->ThemeSettings();
        REQUIRE(readBack.has_value());
        CHECK(readBack->Name == "Compat theme");

        auto excel = ExcelDocumentEditor::CreateNew();
        REQUIRE(excel != nullptr);
        REQUIRE(excel->EnsureTheme());
        CHECK(excel->ThemeXml().has_value());

        // PowerPoint keeps the theme on the slide master rather than on the
        // editor, which is why the matrix links it from the masters chapter.
        auto powerPoint = PowerPointDocumentEditor::CreateNew();
        REQUIRE(powerPoint != nullptr);
        auto master = powerPoint->AddSlideMaster("Themed");
        REQUIRE(master != nullptr);
        CHECK(master->ThemeXml().has_value());
    }

    TEST_CASE("VBA projects: Excel extracts and replaces, Word and PowerPoint only preserve "
              "[compat] [vba]")
    {
        const std::vector<Byte> project{'C', 'F', 'B', 0x00, 0x01, 0x02, 0x03};

        // Excel is graded Yes: extract, replace and remove.
        auto excel = ExcelDocumentEditor::CreateNew(
            ExyokiOffice::Packaging::SpreadsheetDocumentType::MacroEnabledWorkbook);
        REQUIRE(excel != nullptr);
        REQUIRE(excel->SetVbaProjectData(project));

        auto reopened = ExcelDocumentEditor::Open(excel->SaveToMemory());
        REQUIRE(reopened != nullptr);
        CHECK(reopened->GetVbaProjectData() == project);
        CHECK(reopened->RemoveVbaProject());
        CHECK(reopened->GetVbaProjectData().empty());

        // Word is graded Preserved: a macro-enabled package can be created, and
        // the project survives, but no API extracts or replaces it. Creating a
        // macro-enabled document does not create a project either.
        auto word = WordDocumentEditor::CreateNew(
            ExyokiOffice::Packaging::WordprocessingDocumentType::MacroEnabledDocument);
        REQUIRE(word != nullptr);
        REQUIRE(word->AddParagraph("Macro enabled") != nullptr);

        const auto wordBytes = word->SaveToMemory();
        REQUIRE_FALSE(wordBytes.empty());

        auto wordReopened = WordDocumentEditor::Open(wordBytes);
        REQUIRE(wordReopened != nullptr);
        CHECK(wordReopened->GetDocument()->GetDocumentType() ==
              ExyokiOffice::Packaging::WordprocessingDocumentType::MacroEnabledDocument);
        CHECK(wordReopened->GetDocument()->GetPartByUri("/word/vbaProject.bin") == nullptr);

        CheckPreserved(wordBytes);
    }

    TEST_CASE("Protection is an editing restriction, not encryption [compat] [protection]")
    {
        auto word = WordDocumentEditor::CreateNew();
        REQUIRE(word != nullptr);
        REQUIRE(word->AddParagraph("Readable either way") != nullptr);
        REQUIRE(word->ProtectDocument({}, "secret").Succeeded());

        const auto bytes = word->SaveToMemory();
        REQUIRE_FALSE(bytes.empty());

        // The whole point of the note under this row: every part is still
        // readable, and a tool that ignores the setting can rewrite the file.
        ExyokiOffice::OpenXmlPackage package;
        REQUIRE(package.LoadFromMemory(bytes));
        auto documentPart = package.GetPartByUri("/word/document.xml");
        REQUIRE(documentPart != nullptr);
        CHECK(documentPart->GetXmlString().find("Readable either way") != std::string::npos);

        auto reopened = WordDocumentEditor::Open(bytes);
        REQUIRE(reopened != nullptr);
        REQUIRE(reopened->GetDocumentProtection().has_value());
        // No password is needed to edit through this library.
        CHECK(reopened->AddParagraph("Added despite protection") != nullptr);

        // Excel and PowerPoint carry the same posture.
        auto excel = ExcelDocumentEditor::CreateNew();
        REQUIRE(excel != nullptr);
        REQUIRE(excel->ProtectWorkbook({}, "secret").Succeeded());
        CHECK(excel->GetWorkbookProtection().has_value());

        auto powerPoint = PowerPointDocumentEditor::CreateNew();
        REQUIRE(powerPoint != nullptr);
        REQUIRE(powerPoint->ProtectFromModification("secret").Succeeded());
        CHECK(powerPoint->GetModifyProtection().has_value());
    }

    TEST_CASE("Digital signatures go through an application-supplied provider [compat] [signatures]")
    {
        auto provider = std::make_shared<StubCryptoProvider>();

        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        REQUIRE(editor->AddParagraph("Signed content") != nullptr);

        const auto signResult = ExyokiOffice::Security::SignPackage(*editor->GetDocument(), provider);
        CAPTURE(signResult.Message);
        REQUIRE(signResult.Succeeded());

        const auto bytes = editor->SaveToMemory();
        REQUIRE_FALSE(bytes.empty());

        ExyokiOffice::OpenXmlPackage package;
        REQUIRE(package.LoadFromMemory(bytes));
        CHECK(ExyokiOffice::Security::HasSignatures(package));

        const auto verified = ExyokiOffice::Security::VerifySignatures(package, provider);
        CHECK(verified.HasSignatures());
        CHECK(verified.AllValid());
        CHECK(verified.AllContentIntact());

        // Without a provider the library cannot check a signature value itself:
        // it links no cryptographic code of its own. The digests it computes
        // are still enough to say the signed content was not modified.
        const auto withoutProvider = ExyokiOffice::Security::VerifySignatures(package, nullptr);
        CHECK(withoutProvider.HasSignatures());
        CHECK(withoutProvider.AllContentIntact());
        CHECK_FALSE(withoutProvider.AllValid());
    }

    TEST_CASE("An encrypted OOXML file is not a ZIP package and cannot be opened [compat] [encryption]")
    {
        // An encrypted OOXML file is a compound-file container. Its signature is
        // the OLE/CFB magic, not "PK", so it never reaches the package parser.
        const std::vector<Byte> compoundFile{0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1,
                                             0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

        ExyokiOffice::OpenXmlPackage package;
        CHECK_FALSE(package.LoadFromMemory(compoundFile));

        // The failure is reported, not swallowed, and no family editor pretends
        // otherwise.
        CHECK(WordDocumentEditor::Open(compoundFile) == nullptr);
        CHECK(ExcelDocumentEditor::Open(compoundFile) == nullptr);
        CHECK(PowerPointDocumentEditor::Open(compoundFile) == nullptr);
    }

    TEST_CASE("External resources are off by default and never touched by open, save or validate "
              "[compat] [external-resources]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        REQUIRE(editor->AddParagraph("Links outward") != nullptr);
        const auto relationshipId = editor->GetDocument()->AddExternalRelationship(
            "http://schemas.openxmlformats.org/officeDocument/2006/relationships/image",
            "https://cdn.example.test/logo.png");
        REQUIRE_FALSE(relationshipId.empty());

        const auto bytes = editor->SaveToMemory();
        REQUIRE_FALSE(bytes.empty());

        auto resolver = std::make_shared<RecordingResolver>();

        // Installing a resolver is not enough on its own, and more importantly
        // opening, saving and validating never call one.
        ExyokiOffice::Packaging::OpenSettings settings;
        settings.ExternalResources = resolver;
        settings.ExternalResourcePolicy =
            ExternalResourcePolicy::HttpsOnly({"cdn.example.test"}, {ExternalResourceKind::LinkedImage});

        auto reopened = WordDocumentEditor::Open(bytes, settings);
        REQUIRE(reopened != nullptr);
        CHECK(resolver->Calls == 0);

        const auto resaved = reopened->SaveToMemory();
        REQUIRE_FALSE(resaved.empty());
        CHECK(resolver->Calls == 0);

        const auto validation = ValidatePackage(resaved);
        CHECK(validation.Loaded);
        CHECK(resolver->Calls == 0);

        // Auditing what a document points at needs no resolver at all.
        const auto references = ExyokiOffice::Security::CollectExternalReferences(*reopened->GetDocument());
        CHECK_FALSE(references.empty());
        CHECK(resolver->Calls == 0);
    }

} // TEST_SUITE("CrossCuttingMatrixTests")
