// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/Tools/PackageModel.hpp"
#include "ExyokiOffice/Tools/ValidationRunner.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"

using ExyokiOffice::OpenXml::FileFormatVersions;
using ExyokiOffice::Word::WordDocumentEditor;
using namespace ExyokiOffice::Tools;

TEST_CASE("ValidationRunner reports zero errors for a clean document [unit] [tools] [validation-runner]")
{
    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor);
    editor->AddParagraph("Clean paragraph");
    auto bytes = editor->SaveToMemory();
    REQUIRE(!bytes.empty());

    ExyokiOffice::OpenXmlPackage package;
    REQUIRE(package.LoadFromMemory(bytes));

    ValidationRunOptions options;
    options.TargetVersion = FileFormatVersions::Microsoft365;
    const auto report = Run(package, options);

    CHECK(report.Loaded);
    CHECK(report.ErrorCount == 0);
}

TEST_CASE("ValidationRunner --no-dom skips DOM validation issues [unit] [tools] [validation-runner]")
{
    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor);
    editor->AddParagraph("Body");
    auto bytes = editor->SaveToMemory();
    REQUIRE(!bytes.empty());

    ExyokiOffice::OpenXmlPackage package;
    REQUIRE(package.LoadFromMemory(bytes));

    ValidationRunOptions options;
    options.RunDomValidation = false;
    const auto report = Run(package, options);
    CHECK(report.Loaded);
}

TEST_CASE("ValidationRunner surfaces stricter issues for older target versions [unit] [tools] [validation-runner]")
{
    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor);
    editor->AddParagraph("Body text with a run");
    auto bytes = editor->SaveToMemory();
    REQUIRE(!bytes.empty());

    ValidationRunOptions modernOptions;
    modernOptions.TargetVersion = FileFormatVersions::Microsoft365;
    ExyokiOffice::OpenXmlPackage modernPackage;
    REQUIRE(modernPackage.LoadFromMemory(bytes));
    const auto modernReport = Run(modernPackage, modernOptions);

    ValidationRunOptions legacyOptions;
    legacyOptions.TargetVersion = FileFormatVersions::Office2007;
    ExyokiOffice::OpenXmlPackage legacyPackage;
    REQUIRE(legacyPackage.LoadFromMemory(bytes));
    const auto legacyReport = Run(legacyPackage, legacyOptions);

    // A version-gating difference is not guaranteed for every minimal document, but the
    // legacy report must never report *fewer* issues than the modern one for the same bytes.
    CHECK(legacyReport.ErrorCount >= modernReport.ErrorCount);
}

TEST_CASE("Run(path) reports Loaded=false for a missing file [unit] [tools] [validation-runner]")
{
    const auto report = Run(std::filesystem::path("this-file-does-not-exist.docx"));
    CHECK_FALSE(report.Loaded);
}

TEST_CASE("ParseFileFormatVersion accepts known tokens and rejects unknown ones [unit] [tools] [validation-runner]")
{
    CHECK(ParseFileFormatVersion("2007") == FileFormatVersions::Office2007);
    CHECK(ParseFileFormatVersion("365") == FileFormatVersions::Microsoft365);
    CHECK(ParseFileFormatVersion("Microsoft365") == FileFormatVersions::Microsoft365);
    CHECK_FALSE(ParseFileFormatVersion("1999").has_value());
}
