// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "TestSupport.hpp"

#include "ExyokiOffice/Tools/DocumentModelIO.hpp"
#include "ExyokiOffice/Tools/PackageArchiver.hpp"
#include "ExyokiOffice/Tools/PackageDiff.hpp"
#include "ExyokiOffice/Tools/PackageLimits.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <optional>
#include <vector>

using ExyokiOffice::Word::WordDocumentEditor;
using namespace ExyokiOffice::Tools;

namespace
{

std::filesystem::path MakeTemporaryPath(std::string_view stem, std::string_view extension = "")
{
    return ExyokiOfficeTests::MakeTemporaryPath(stem, extension);
}

std::filesystem::path MakeSampleDocx()
{
    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor);
    editor->AddParagraph("Round-trip fidelity paragraph.");
    const auto path = MakeTemporaryPath("exyoki_archiver", ".docx");
    REQUIRE(editor->SaveToFile(path));
    return path;
}

bool HasDiagnostic(const std::vector<ToolDiagnostic>& diagnostics, ToolSeverity severity, std::string_view text)
{
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [severity, text](const ToolDiagnostic& diagnostic)
                       {
                           return diagnostic.Severity == severity &&
                                  diagnostic.Message.find(text) != std::string::npos;
                       });
}

} // namespace

TEST_CASE("Unpack followed by Pack reproduces byte-identical ZIP entries [unit] [tools]")
{
    const auto docxPath = MakeSampleDocx();
    const auto outDir = MakeTemporaryPath("exyoki_unpack_dir");
    const auto repackedPath = MakeTemporaryPath("exyoki_repacked", ".docx");

    const auto unpackResult = Unpack(docxPath, outDir);
    CHECK(unpackResult.Ok);
    CHECK(unpackResult.EntryCount > 0);
    CHECK_FALSE(unpackResult.ManifestWritten);

    const auto packResult = Pack(outDir, repackedPath);
    CHECK(packResult.Ok);
    CHECK(packResult.EntryCount == unpackResult.EntryCount);

    const auto diff = Compare(docxPath, repackedPath);
    CHECK(diff.Ok);
    CHECK(diff.Identical);

    std::filesystem::remove_all(outDir);
    std::filesystem::remove(docxPath);
    std::filesystem::remove(repackedPath);
}

TEST_CASE("Unpack --pretty round-trips a document that still loads validly [unit] [tools]")
{
    const auto docxPath = MakeSampleDocx();
    const auto outDir = MakeTemporaryPath("exyoki_pretty_dir");
    const auto repackedPath = MakeTemporaryPath("exyoki_pretty_repacked", ".docx");

    UnpackOptions options;
    options.PrettyPrintXml = true;
    const auto unpackResult = Unpack(docxPath, outDir, options);
    CHECK(unpackResult.Ok);
    CHECK(unpackResult.ManifestWritten);

    const auto packResult = Pack(outDir, repackedPath);
    CHECK(packResult.Ok);

    auto reopened = WordDocumentEditor::Open(repackedPath);
    CHECK(reopened != nullptr);

    std::filesystem::remove_all(outDir);
    std::filesystem::remove(docxPath);
    std::filesystem::remove(repackedPath);
}

TEST_CASE("Pack regenerates [Content_Types].xml when requested or missing [unit] [tools]")
{
    const auto docxPath = MakeSampleDocx();
    const auto outDir = MakeTemporaryPath("exyoki_regen_dir");
    Unpack(docxPath, outDir);

    const auto repackedPath = MakeTemporaryPath("exyoki_regen_repacked", ".docx");
    PackOptions options;
    options.RegenerateContentTypes = true;
    const auto packResult = Pack(outDir, repackedPath, options);
    CHECK(packResult.Ok);
    CHECK(packResult.ContentTypesRegenerated);

    std::filesystem::remove_all(outDir);
    std::filesystem::remove(docxPath);
    std::filesystem::remove(repackedPath);
}

TEST_CASE("Pack rejects a missing input directory [unit] [tools]")
{
    const auto repackedPath = MakeTemporaryPath("exyoki_missing_repacked", ".docx");
    const auto result = Pack(MakeTemporaryPath("exyoki_does_not_exist"), repackedPath);
    CHECK_FALSE(result.Ok);
    CHECK(!result.Diagnostics.empty());
}

TEST_CASE("The Tools layer defaults to the recommended limits [unit] [tools] [limits]")
{
    // Nothing configured this process, so the module supplies its own default
    // rather than inheriting the library's Unlimited(): these entry points are
    // pointed at files the caller did not produce, and most take no settings at
    // all.
    REQUIRE_FALSE(ExyokiOffice::OpenXmlPackage::ConfiguredDefaultPackageLimits().has_value());

    const auto recommended = ExyokiOffice::OpenXmlPackageLimits::Recommended();
    CHECK(DefaultPackageLimits().MaxEntries == recommended.MaxEntries);
    CHECK(DefaultPackageLimits().MaxCompressionRatio == recommended.MaxCompressionRatio);
    CHECK(UnpackOptions{}.Limits.MaxEntries == recommended.MaxEntries);
    CHECK(ValidationRunOptions{}.Limits.MaxXmlDepth == recommended.MaxXmlDepth);

    const auto previous = ExyokiOffice::OpenXmlPackage::ConfiguredDefaultPackageLimits();
    struct Restore
    {
        std::optional<ExyokiOffice::OpenXmlPackageLimits> Value;
        ~Restore() { ExyokiOffice::OpenXmlPackage::SetDefaultPackageLimits(Value); }
    } const restore{previous};

    // An application that installed a policy gets that policy, including a
    // deliberate Unlimited() — `exyoki --package-limits unlimited` has to reach
    // here, or it would not be an escape hatch.
    ExyokiOffice::OpenXmlPackage::SetDefaultPackageLimits(ExyokiOffice::OpenXmlPackageLimits::Unlimited());
    CHECK(DefaultPackageLimits().MaxEntries == 0);
    CHECK(UnpackOptions{}.Limits.MaxEntries == 0);
}

TEST_CASE("A Tools entry point that opens an editor is held to the Tools limits [unit] [tools] [limits]")
{
    // The gap this closes: ReadWordModel reaches the package only through
    // WordDocumentEditor::Open, with no OpenXmlPackage probe in front of it,
    // and that Open used to take a default-constructed OpenSettings - so it ran
    // Unlimited whatever this layer had decided.
    const auto docxPath = MakeSampleDocx();

    // Reads normally before any policy is installed, so the refusal below is
    // the policy and not the document.
    {
        std::vector<ToolDiagnostic> diagnostics;
        const auto model = ReadWordModel(docxPath, {}, diagnostics);
        CHECK(model.Family == DocumentFamily::Word);
        CHECK(diagnostics.empty());
    }

    const auto previous = ExyokiOffice::OpenXmlPackage::ConfiguredDefaultPackageLimits();
    struct Restore
    {
        std::optional<ExyokiOffice::OpenXmlPackageLimits> Value;
        ~Restore() { ExyokiOffice::OpenXmlPackage::SetDefaultPackageLimits(Value); }
    } const restore{previous};

    auto tight = ExyokiOffice::OpenXmlPackageLimits::Unlimited();
    tight.MaxEntries = 1;
    ExyokiOffice::OpenXmlPackage::SetDefaultPackageLimits(tight);

    std::vector<ToolDiagnostic> diagnostics;
    const auto model = ReadWordModel(docxPath, {}, diagnostics);
    CHECK(model.Family == DocumentFamily::Unknown);
    CHECK_FALSE(diagnostics.empty());

    std::filesystem::remove(docxPath);
}

TEST_CASE("Unpack refuses an entry that exceeds the configured size limit [unit] [tools] [limits]")
{
    const auto docxPath = MakeSampleDocx();
    const auto outDir = MakeTemporaryPath("exyoki_limit_dir");

    // Unpack reads the archive itself rather than through OpenXmlPackage, so
    // this is checking its own guard and not the loader's.
    UnpackOptions options;
    options.Limits = ExyokiOffice::OpenXmlPackageLimits::Unlimited();
    options.Limits.MaxPartBytes = 16;

    const auto result = Unpack(docxPath, outDir, options);
    CHECK_FALSE(result.Ok);
    CHECK(HasDiagnostic(result.Diagnostics, ToolSeverity::Error, "part size limit"));

    std::filesystem::remove_all(outDir);
    std::filesystem::remove(docxPath);
}

TEST_CASE("Unpack refuses an archive with more entries than the limit allows [unit] [tools] [limits]")
{
    const auto docxPath = MakeSampleDocx();
    const auto outDir = MakeTemporaryPath("exyoki_entrylimit_dir");

    UnpackOptions options;
    options.Limits = ExyokiOffice::OpenXmlPackageLimits::Unlimited();
    options.Limits.MaxEntries = 1;

    const auto result = Unpack(docxPath, outDir, options);
    CHECK_FALSE(result.Ok);
    CHECK(HasDiagnostic(result.Diagnostics, ToolSeverity::Error, "entry count limit"));
    // Refused from the ZIP directory, so nothing was extracted at all.
    CHECK(result.EntryCount == 0);

    std::filesystem::remove_all(outDir);
    std::filesystem::remove(docxPath);
}

TEST_CASE("Unpack keeps a name that merely contains dots [unit] [tools]")
{
    // The traversal check is per component; a substring search for ".." would
    // refuse this legitimate name.
    const auto docxPath = MakeSampleDocx();
    const auto outDir = MakeTemporaryPath("exyoki_dots_dir");
    const auto repackedPath = MakeTemporaryPath("exyoki_dots_repacked", ".docx");

    REQUIRE(Unpack(docxPath, outDir).Ok);

    std::filesystem::create_directories(outDir / "custom");
    {
        std::ofstream extra(outDir / "custom" / "..data..xml", std::ios::trunc);
        extra << "<data/>";
    }

    REQUIRE(Pack(outDir, repackedPath).Ok);

    const auto checkDir = MakeTemporaryPath("exyoki_dots_check");
    const auto recheck = Unpack(repackedPath, checkDir);
    CHECK(recheck.Ok);
    CHECK_FALSE(HasDiagnostic(recheck.Diagnostics, ToolSeverity::Error, "path traversal"));

    std::filesystem::remove_all(outDir);
    std::filesystem::remove_all(checkDir);
    std::filesystem::remove(docxPath);
    std::filesystem::remove(repackedPath);
}

TEST_CASE("Pack ignores a manifest that would restore an escaping entry name [unit] [tools]")
{
    const auto docxPath = MakeSampleDocx();
    const auto outDir = MakeTemporaryPath("exyoki_manifest_dir");
    const auto repackedPath = MakeTemporaryPath("exyoki_manifest_repacked", ".docx");

    REQUIRE(Unpack(docxPath, outDir).Ok);

    // The manifest lives in the input tree, so it is exactly as trustworthy as
    // whoever produced that tree. Honoring this one would make Pack a generator
    // of traversal archives for whatever extractor opens the result next.
    {
        std::ofstream manifest(outDir / std::filesystem::path(kArchiverManifestFileName), std::ios::trunc);
        manifest << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                    "<exyokiUnpackManifest prettyPrinted=\"false\">\n"
                    "  <rename from=\"../../../evil.xml\" to=\"word/document.xml\"/>\n"
                    "</exyokiUnpackManifest>\n";
    }

    const auto packResult = Pack(outDir, repackedPath);
    CHECK(packResult.Ok);
    CHECK(HasDiagnostic(packResult.Diagnostics, ToolSeverity::Warning, "usable ZIP entry name"));

    // The entry kept its on-disk name, so the result is still a document rather
    // than an archive aimed outside its output directory.
    const auto checkDir = MakeTemporaryPath("exyoki_manifest_check");
    const auto recheck = Unpack(repackedPath, checkDir);
    CHECK(recheck.Ok);
    CHECK_FALSE(HasDiagnostic(recheck.Diagnostics, ToolSeverity::Error, "path traversal"));
    CHECK(std::filesystem::exists(checkDir / "word" / "document.xml"));

    std::filesystem::remove_all(outDir);
    std::filesystem::remove_all(checkDir);
    std::filesystem::remove(docxPath);
    std::filesystem::remove(repackedPath);
}
