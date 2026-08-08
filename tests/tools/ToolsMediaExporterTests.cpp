// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "TestSupport.hpp"

#include "ExyokiOffice/Tools/FlatOpcConverter.hpp"
#include "ExyokiOffice/Tools/MediaExporter.hpp"
#include "ExyokiOffice/Tools/OutputNaming.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <chrono>
#include <string>

using ExyokiOffice::MeasurementUnit;
using ExyokiOffice::MeasuringUnits;
using ExyokiOffice::Word::WordDocumentEditor;
using namespace ExyokiOffice::Tools;

namespace
{

std::filesystem::path MakeTemporaryPath(std::string_view stem)
{
    return ExyokiOfficeTests::MakeTemporaryPath(stem, "");
}

/// Minimal valid 1x1 transparent PNG (recognized by Word::DetectImageFormat).
std::vector<ExyokiOffice::Byte> MinimalPng()
{
    return {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44,
            0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F,
            0x15, 0xC4, 0x89, 0x00, 0x00, 0x00, 0x0A, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x00,
            0x01, 0x00, 0x00, 0x05, 0x00, 0x01, 0x0D, 0x0A, 0x2D, 0xB4, 0x00, 0x00, 0x00, 0x00, 0x49,
            0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
}

/**
 * @brief A package whose one media part carries @p partFileName.
 *
 * Going through Flat OPC is what makes the part name choosable: the package is
 * one XML document there, so renaming the part means renaming it in the
 * `pkg:name` attribute and in the relationship that reaches it, and
 * ConvertFromFlatOpc rebuilds a genuine archive around the result.
 */
std::vector<ExyokiOffice::Byte> PackageWithMediaNamed(std::string_view partFileName)
{
    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor);
    REQUIRE(editor->AddImageFromData(MinimalPng(), "image/png", MeasuringUnits(1.0, MeasurementUnit::Inch),
                                     MeasuringUnits(1.0, MeasurementUnit::Inch)));

    const auto flat = ConvertToFlatOpc(editor->SaveToMemory());
    REQUIRE(flat.Ok);

    static constexpr std::string_view mediaFolder = "/word/media/";
    const auto folder = flat.FlatOpcXml.find(mediaFolder);
    REQUIRE(folder != std::string::npos);
    const auto nameStart = folder + mediaFolder.size();
    const auto nameEnd = flat.FlatOpcXml.find('"', nameStart);
    REQUIRE(nameEnd != std::string::npos);
    const auto originalName = flat.FlatOpcXml.substr(nameStart, nameEnd - nameStart);

    // The relationship target is relative ("media/image1.png"), so replacing the
    // file name alone keeps the part reachable under its new name.
    std::string renamed = flat.FlatOpcXml;
    for (auto position = renamed.find(originalName); position != std::string::npos;
         position = renamed.find(originalName, position + partFileName.size()))
    {
        renamed.replace(position, originalName.size(), partFileName);
    }

    const auto rebuilt = ConvertFromFlatOpc(renamed);
    REQUIRE(rebuilt.Ok);
    return rebuilt.PackageBytes;
}

} // namespace

TEST_CASE("ExportMedia exports an embedded PNG with the correct extension [unit] [tools]")
{
    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor);
    editor->AddParagraph("Before image");
    auto image = editor->AddImageFromData(MinimalPng(), "image/png", MeasuringUnits(1.0, MeasurementUnit::Inch),
                                          MeasuringUnits(1.0, MeasurementUnit::Inch));
    REQUIRE(image);

    auto bytes = editor->SaveToMemory();
    REQUIRE(!bytes.empty());
    ExyokiOffice::OpenXmlPackage package;
    REQUIRE(package.LoadFromMemory(bytes));

    const auto outDir = MakeTemporaryPath("exyoki_media");
    const auto result = ExportMedia(package, outDir);
    CHECK(result.Ok);
    REQUIRE(result.Items.size() == 1);
    CHECK(result.Items[0].OutputPath.extension() == ".png");
    CHECK(std::filesystem::exists(result.Items[0].OutputPath));
    CHECK(result.Items[0].Size == MinimalPng().size());

    std::filesystem::remove_all(outDir);
}

TEST_CASE("ExportMedia resolves name collisions with a numeric suffix [unit] [tools]")
{
    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor);
    editor->AddImageFromData(MinimalPng(), "image/png", MeasuringUnits(1.0, MeasurementUnit::Inch),
                             MeasuringUnits(1.0, MeasurementUnit::Inch));
    editor->AddImageFromData(MinimalPng(), "image/png", MeasuringUnits(1.0, MeasurementUnit::Inch),
                             MeasuringUnits(1.0, MeasurementUnit::Inch));

    auto bytes = editor->SaveToMemory();
    REQUIRE(!bytes.empty());
    ExyokiOffice::OpenXmlPackage package;
    REQUIRE(package.LoadFromMemory(bytes));

    const auto outDir = MakeTemporaryPath("exyoki_media_collision");
    const auto result = ExportMedia(package, outDir);
    CHECK(result.Ok);
    CHECK(result.Items.size() == 2);

    std::vector<std::string> names;
    for (const auto& item : result.Items)
    {
        names.push_back(item.OutputPath.filename().string());
    }
    CHECK(names[0] != names[1]);

    std::filesystem::remove_all(outDir);
}

TEST_CASE("ExportMedia does not take an output name from a hostile part URI [unit] [tools]")
{
    // The document is the untrusted input here: both halves of the output name
    // come from the part URI, so a part called NUL.png would write to the
    // console device - reporting success and storing nothing - and one with a
    // stream suffix would hide the payload inside a file that looks ordinary.
    std::string partFileName;
    SUBCASE("a device name")
    {
        partFileName = "NUL.png";
    }
    SUBCASE("an alternate data stream")
    {
        // The colon sits before the extension, so what is written still ends in
        // ".png" and the stream is what receives the bytes.
        partFileName = "image1:hidden.png";
    }
    // Only spellings that survive a package round-trip are exercised here: a
    // part name carrying `..` is refused by the Flat OPC importer, and a
    // trailing space does not reach the exporter. Those rules are covered
    // directly in ToolsOutputNamingTests.cpp.

    ExyokiOffice::OpenXmlPackage package;
    REQUIRE(package.LoadFromMemory(PackageWithMediaNamed(partFileName)));

    const auto outDir = MakeTemporaryPath("exyoki_media_hostile");
    const auto result = ExportMedia(package, outDir);
    CHECK(result.Ok);
    REQUIRE(result.Items.size() == 1);

    const auto& written = result.Items[0].OutputPath;
    CHECK(IsPlainOutputName(written.filename().string()));
    CHECK(IsInsideDirectory(outDir, written));
    CHECK(written.filename().string() != partFileName);

    // The payload is where the caller was told it is, at its real size: the
    // device would have accepted the bytes and kept none of them.
    REQUIRE(std::filesystem::exists(written));
    CHECK(std::filesystem::file_size(written) == MinimalPng().size());

    // The rename is reported rather than silent - the name came from the
    // document, and the caller may well want to know the document carried it.
    const auto renameReported =
        std::any_of(result.Diagnostics.begin(), result.Diagnostics.end(),
                    [](const ToolDiagnostic& diagnostic)
                    { return diagnostic.Message.find("not usable as a file name") != std::string::npos; });
    CHECK(renameReported);

    std::filesystem::remove_all(outDir);
}
