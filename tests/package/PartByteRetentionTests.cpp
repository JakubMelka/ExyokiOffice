// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace
{
using ExyokiOffice::OpenXmlPackage;
using ExyokiOffice::PartByteRetention;
using ExyokiOffice::Word::WordDocumentEditor;

std::vector<ExyokiOffice::Byte> BuildDocument()
{
    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor);
    editor->AddParagraph("Retention");
    auto bytes = editor->SaveToMemory();
    REQUIRE(!bytes.empty());
    return bytes;
}

std::string ToString(const std::vector<ExyokiOffice::Byte>& bytes)
{
    return std::string(bytes.begin(), bytes.end());
}
} // namespace

TEST_SUITE("Part byte retention")
{

    TEST_CASE("Always keeps the bytes read for XML parts [unit] [package] [retention]")
    {
        const auto bytes = BuildDocument();

        OpenXmlPackage package;
        package.SetPartByteRetention(PartByteRetention::Always);
        CHECK(package.GetPartByteRetention() == PartByteRetention::Always);
        REQUIRE(package.LoadFromMemory(bytes));

        auto part = package.GetPartByUri("/word/document.xml");
        REQUIRE(part);
        CHECK(part->HasOriginalBytes());

        const auto original = ToString(part->GetOriginalBytes());
        CHECK(!original.empty());
        CHECK(original.find("<w:document") != std::string::npos);
        CHECK(original == part->GetXmlString());
    }

    TEST_CASE("Retained bytes survive a later edit of the part [unit] [package] [retention]")
    {
        const auto bytes = BuildDocument();

        OpenXmlPackage package;
        package.SetPartByteRetention(PartByteRetention::Always);
        REQUIRE(package.LoadFromMemory(bytes));

        auto part = package.GetPartByUri("/word/document.xml");
        REQUIRE(part);
        const auto original = ToString(part->GetOriginalBytes());

        part->SetXmlString("<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"/>");
        CHECK(part->GetXmlString() != original);
        CHECK(ToString(part->GetOriginalBytes()) == original);
    }

    TEST_CASE("Never drops the bytes read for every part [unit] [package] [retention]")
    {
        const auto bytes = BuildDocument();

        OpenXmlPackage package;
        package.SetPartByteRetention(PartByteRetention::Never);
        REQUIRE(package.LoadFromMemory(bytes));

        auto part = package.GetPartByUri("/word/document.xml");
        REQUIRE(part);
        CHECK_FALSE(part->HasOriginalBytes());
        CHECK(part->GetOriginalBytes().empty());
    }

    TEST_CASE("The default policy skips packages without signatures [unit] [package] [retention]")
    {
        const auto bytes = BuildDocument();

        OpenXmlPackage package;
        CHECK(package.GetPartByteRetention() == PartByteRetention::WhenSignaturesPresent);
        REQUIRE(package.LoadFromMemory(bytes));

        auto part = package.GetPartByUri("/word/document.xml");
        REQUIRE(part);
        CHECK_FALSE(part->HasOriginalBytes());
    }

    TEST_CASE("Parts created in memory have no original bytes [unit] [package] [retention]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor);
        editor->AddParagraph("Fresh");

        auto part = editor->GetDocument()->GetPartByUri("/word/document.xml");
        REQUIRE(part);
        CHECK_FALSE(part->HasOriginalBytes());
    }

} // TEST_SUITE
