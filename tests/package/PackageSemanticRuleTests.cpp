// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

// The package-semantic rules behind S-5 in MCP_ERRORS.md: files that validated
// with zero errors while Word, Excel or PowerPoint refused them. Every case
// builds the offending package through the editor or package API - the shapes
// the library itself can produce - and checks the rule by its identifier, then
// shows that the correct package does not trigger it.

#include "doctest.h"

#include "TestSupport.hpp"

#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/OpenXmlPackageValidator.hpp"
#include "ExyokiOffice/Packaging/PowerPointDocument.hpp"
#include "ExyokiOffice/Packaging/SpreadsheetDocument.hpp"
#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"

#include <algorithm>
#include <cctype>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using ExyokiOffice::ValidationErrorId;
using ExyokiOffice::ValidationIssue;
using ExyokiOffice::ValidationResult;
using ExyokiOffice::ValidationSeverity;

std::vector<ValidationIssue> IssuesWithId(const ValidationResult& result, ValidationErrorId id)
{
    std::vector<ValidationIssue> issues;
    std::copy_if(result.Issues().begin(), result.Issues().end(), std::back_inserter(issues), [id](const auto& issue)
                 { return issue.Id == id; });
    return issues;
}

/// Reloads saved bytes and validates them the way `exyoki validate` and the
/// MCP `validate_document` tool do: once OPC-only, once with DOM validation.
/// The semantic rules have to fire in both, so both results are merged.
ValidationResult ValidateSaved(std::span<const ExyokiOffice::Byte> bytes, bool withDomValidation)
{
    ExyokiOffice::OpenXmlPackage package;
    REQUIRE(package.LoadFromMemory(bytes));
    if (withDomValidation)
    {
        return ExyokiOffice::OpenXmlPackageValidator(ExyokiOffice::OpenXmlDomValidationSettings{}).Validate(package);
    }
    return ExyokiOffice::OpenXmlPackageValidator().Validate(package);
}

ExyokiOffice::Excel::CellRange Range(std::string_view text)
{
    const auto value = ExyokiOffice::Excel::CellRange::ParseA1(text);
    REQUIRE(value);
    return *value;
}

constexpr std::string_view kSpreadsheetNamespace = "http://schemas.openxmlformats.org/spreadsheetml/2006/main";

std::string TableXml(std::string_view id, std::string_view name, std::string_view reference)
{
    std::string xml = R"(<?xml version="1.0" encoding="UTF-8"?><x:table xmlns:x=")";
    xml += kSpreadsheetNamespace;
    xml += R"(" id=")";
    xml += id;
    xml += R"(" name=")";
    xml += name;
    xml += R"(" displayName=")";
    xml += name;
    xml += R"(" ref=")";
    xml += reference;
    xml += R"("><x:tableColumns count="2"><x:tableColumn id="1" name="First"/><x:tableColumn id="2" name="Second"/></x:tableColumns></x:table>)";
    return xml;
}

} // namespace

TEST_SUITE("PackageSemanticRuleTests")
{

    TEST_CASE("S-5a: a table reference pointing at no relationship of the worksheet is an error [opc][validation][semantic][spreadsheet] [unit] [opc-validation]")
    {
        using namespace ExyokiOffice::Excel;

        auto editor = ExcelDocumentEditor::CreateNew();
        REQUIRE(editor);
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        const auto table = sheet->CreateTable("Sales", Range("A1:C5"), {{0, "Product"}, {0, "Quantity"}, {0, "Amount"}});
        REQUIRE(table);
        const auto relationshipId = table->GetPart()->RelationshipId();
        REQUIRE_FALSE(relationshipId.empty());

        SUBCASE("the intact workbook is accepted")
        {
            const auto result = ValidateSaved(editor->SaveToMemory(), false);
            CHECK(IssuesWithId(result, ValidationErrorId::PackageDanglingRelationshipReference).empty());
            CHECK_FALSE(result.HasErrors());
        }

        SUBCASE("removing the relationship leaves x:tablePart pointing nowhere")
        {
            // The shape X-1 produced: the worksheet XML still says
            // <x:tablePart r:id="rIdN"/>, the part has no such relationship,
            // and no generated schematron rule covers x:tablePart.
            REQUIRE(sheet->GetPart()->RemovePartReference(table->GetPart()));
            const auto bytes = editor->SaveToMemory();
            REQUIRE_FALSE(bytes.empty());

            for (const bool withDom : {false, true})
            {
                CAPTURE(withDom);
                const auto result = ValidateSaved(bytes, withDom);
                const auto issues = IssuesWithId(result, ValidationErrorId::PackageDanglingRelationshipReference);
                REQUIRE(issues.size() == 1);
                CHECK_FALSE(result.IsValid());
                CHECK(issues.front().Severity == ValidationSeverity::Error);
                CHECK(issues.front().Domain == ExyokiOffice::ValidationDomain::Packaging);
                CHECK(issues.front().PartUri == sheet->GetPart()->Uri());
                CHECK(issues.front().RelationshipSourceUri == sheet->GetPart()->Uri());
                CHECK(issues.front().RelationshipId == relationshipId);
                CHECK(issues.front().Location.ElementName.find("tablePart") != std::string::npos);
                CHECK(issues.front().Location.AttributeName.find("id") != std::string::npos);
                CHECK(issues.front().Message.find(relationshipId) != std::string::npos);
            }
        }
    }

    TEST_CASE("S-5a: relationship attributes are recognized by namespace, whatever prefix binds it [opc][validation][semantic][spreadsheet] [unit] [opc-validation]")
    {
        // The document binds the relationships namespace to `rel`, not `r`.
        // A rule matching on the prefix would read nothing here.
        auto workbook = ExyokiOffice::Packaging::ExcelDocument::Create();
        REQUIRE(workbook);
        auto workbookPart = workbook->AddWorkbookPart();
        REQUIRE(workbookPart);
        auto sheet = workbookPart->AddWorksheetPart();
        REQUIRE(sheet);
        sheet->SetXmlString(
            R"(<?xml version="1.0" encoding="UTF-8"?>
<x:worksheet xmlns:x="http://schemas.openxmlformats.org/spreadsheetml/2006/main"
             xmlns:rel="http://schemas.openxmlformats.org/officeDocument/2006/relationships">
  <x:sheetData/>
  <x:tableParts count="1"><x:tablePart rel:id="rIdMissing"/></x:tableParts>
</x:worksheet>)");

        const auto result = ExyokiOffice::OpenXmlPackageValidator().Validate(*workbook);
        const auto issues = IssuesWithId(result, ValidationErrorId::PackageDanglingRelationshipReference);
        REQUIRE(issues.size() == 1);
        CHECK(issues.front().PartUri == sheet->Uri());
        CHECK(issues.front().RelationshipId == "rIdMissing");

        // An empty reference is not a reference: Office writes r:id="" for
        // hyperlinks that only carry an action, and refuses nothing.
        sheet->SetXmlString(
            R"(<?xml version="1.0" encoding="UTF-8"?>
<x:worksheet xmlns:x="http://schemas.openxmlformats.org/spreadsheetml/2006/main"
             xmlns:rel="http://schemas.openxmlformats.org/officeDocument/2006/relationships">
  <x:sheetData/>
  <x:tableParts count="1"><x:tablePart rel:id=""/></x:tableParts>
</x:worksheet>)");
        CHECK(IssuesWithId(ExyokiOffice::OpenXmlPackageValidator().Validate(*workbook),
                           ValidationErrorId::PackageDanglingRelationshipReference)
                  .empty());
    }

    TEST_CASE("S-5a: an element a schematron rule covers reports a missing relationship once, under the same id [opc][validation][semantic][word] [unit] [opc-validation]")
    {
        auto document = ExyokiOffice::Packaging::WordDocument::Create(
            ExyokiOffice::Packaging::WordprocessingDocumentType::Document);
        REQUIRE(document);
        auto main = document->AddMainDocumentPart();
        REQUIRE(main);
        main->SetXmlString(
            R"(<?xml version="1.0" encoding="UTF-8"?>
<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"
            xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">
  <w:body><w:p><w:hyperlink r:id="rIdGone"><w:r><w:t>link</w:t></w:r></w:hyperlink></w:p></w:body>
</w:document>)");

        const auto result = ExyokiOffice::OpenXmlPackageValidator().Validate(*document);
        const auto issues = IssuesWithId(result, ValidationErrorId::PackageDanglingRelationshipReference);
        REQUIRE(issues.size() == 1);
        CHECK(issues.front().RelationshipId == "rIdGone");
        CHECK(issues.front().PartUri == main->Uri());
        // Nothing else complained about the same attribute under another id.
        CHECK(std::count_if(result.Issues().begin(), result.Issues().end(), [](const ValidationIssue& issue)
                            { return issue.Message.find("rIdGone") != std::string::npos; }) == 1);
    }

    TEST_CASE("S-5b: two table parts of one worksheet whose ranges overlap are an error [opc][validation][semantic][spreadsheet] [unit] [opc-validation]")
    {
        using namespace ExyokiOffice::Excel;

        auto editor = ExcelDocumentEditor::CreateNew();
        REQUIRE(editor);
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        const auto first = sheet->CreateTable("TblOne", Range("A1:B3"), {{0, "Alpha"}, {0, "Beta"}});
        REQUIRE(first);

        // The second table part is attached by hand: the X-2 shape, and the
        // editor is meant to refuse it once that defect is fixed.
        auto second = sheet->GetPart()->AddTableDefinitionPart();
        REQUIRE(second);

        SUBCASE("overlapping cells are refused")
        {
            second->SetXmlString(TableXml("2", "TblTwo", "B2:C4"));
            const auto result = ValidateSaved(editor->SaveToMemory(), false);
            const auto issues = IssuesWithId(result, ValidationErrorId::PackageTableRangeOverlap);
            REQUIRE(issues.size() == 1);
            CHECK_FALSE(result.IsValid());
            CHECK(issues.front().Severity == ValidationSeverity::Error);
            CHECK(issues.front().PartUri == sheet->GetPart()->Uri());
            CHECK(issues.front().TargetUri == second->Uri());
            CHECK(issues.front().Message.find("A1:B3") != std::string::npos);
            CHECK(issues.front().Message.find("B2:C4") != std::string::npos);
            CHECK(issues.front().Message.find(first->GetPart()->Uri()) != std::string::npos);
        }

        SUBCASE("a table beside the first is accepted")
        {
            second->SetXmlString(TableXml("2", "TblTwo", "D1:E3"));
            const auto result = ValidateSaved(editor->SaveToMemory(), false);
            CHECK(IssuesWithId(result, ValidationErrorId::PackageTableRangeOverlap).empty());
        }

        SUBCASE("touching edges do not overlap")
        {
            second->SetXmlString(TableXml("2", "TblTwo", "C1:D3"));
            const auto result = ValidateSaved(editor->SaveToMemory(), false);
            CHECK(IssuesWithId(result, ValidationErrorId::PackageTableRangeOverlap).empty());
        }
    }

    TEST_CASE("S-5c: a threaded comment whose person the workbook does not list is an error [opc][validation][semantic][spreadsheet] [unit] [opc-validation]")
    {
        using namespace ExyokiOffice::Excel;

        auto editor = ExcelDocumentEditor::CreateNew();
        REQUIRE(editor);
        auto worksheet = editor->Worksheets().front();
        REQUIRE(worksheet);

        ExcelThreadedComment comment;
        comment.Address = *CellAddress::ParseA1("A1");
        comment.PersonName = "Jane Reviewer";
        comment.Text = "Check this number";
        REQUIRE(worksheet->AddThreadedComment(comment));

        auto workbook = editor->GetDocument()->GetWorkbookPart();
        REQUIRE(workbook);
        const auto persons = workbook->GetWorkbookPersonParts();
        REQUIRE(persons.size() == 1);

        SUBCASE("the workbook with its person list is accepted")
        {
            const auto result = ValidateSaved(editor->SaveToMemory(), false);
            CHECK(IssuesWithId(result, ValidationErrorId::PackageThreadedCommentPersonUndefined).empty());
            CHECK_FALSE(result.HasErrors());
        }

        SUBCASE("without the person list every personId points nowhere")
        {
            // The X-3 shape: the comment part travelled, the person list did not.
            REQUIRE(workbook->RemoveWorkbookPersonPart(persons.front()));
            const auto bytes = editor->SaveToMemory();
            REQUIRE_FALSE(bytes.empty());

            for (const bool withDom : {false, true})
            {
                CAPTURE(withDom);
                const auto result = ValidateSaved(bytes, withDom);
                const auto issues = IssuesWithId(result, ValidationErrorId::PackageThreadedCommentPersonUndefined);
                REQUIRE(issues.size() == 1);
                CHECK_FALSE(result.IsValid());
                CHECK(issues.front().Severity == ValidationSeverity::Error);
                std::string partUri = issues.front().PartUri;
                std::transform(partUri.begin(), partUri.end(), partUri.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                CHECK(partUri.find("threadedcomment") != std::string::npos);
                CHECK(issues.front().Message.find("no person list") != std::string::npos);
                CHECK(issues.front().Location.ElementName.find("threadedComment") != std::string::npos);
            }
        }

        SUBCASE("a person list that does not know the author is not enough")
        {
            persons.front()->SetXmlString(
                R"(<?xml version="1.0" encoding="UTF-8"?>
<personList xmlns="http://schemas.microsoft.com/office/spreadsheetml/2018/threadedcomments">
  <person displayName="Somebody Else" id="{11111111-2222-3333-4444-555555555555}" userId="somebody" providerId="None"/>
</personList>)");
            const auto result = ValidateSaved(editor->SaveToMemory(), false);
            const auto issues = IssuesWithId(result, ValidationErrorId::PackageThreadedCommentPersonUndefined);
            REQUIRE(issues.size() == 1);
            CHECK(issues.front().Message.find("no person list related from the workbook defines") != std::string::npos);
        }
    }

    TEST_CASE("S-5d: a presentation without the master, layout and theme chain is an error [opc][validation][semantic][powerpoint] [unit] [opc-validation]")
    {
        // Built through the package API, one part at a time, so every link of
        // the chain PowerPoint requires can be left out on purpose.
        auto document = ExyokiOffice::Packaging::PowerPointDocument::Create();
        REQUIRE(document);
        auto presentation = document->AddPresentationPart();
        REQUIRE(presentation);
        auto slide = presentation->AddSlidePart();
        REQUIRE(slide);

        SUBCASE("no master at all, and a slide without a layout")
        {
            // The P-2 shape: presentation.xml with no p:sldMasterIdLst, a slide
            // with no relationships.
            const auto result = ExyokiOffice::OpenXmlPackageValidator().Validate(*document);
            const auto missingMaster = IssuesWithId(result, ValidationErrorId::PackagePresentationMissingSlideMaster);
            REQUIRE(missingMaster.size() == 1);
            CHECK(missingMaster.front().Severity == ValidationSeverity::Error);
            CHECK(missingMaster.front().PartUri == presentation->Uri());
            CHECK(missingMaster.front().Message.find("sldMasterIdLst") != std::string::npos);

            const auto missingLayout = IssuesWithId(result, ValidationErrorId::PackageSlideMissingSlideLayout);
            REQUIRE(missingLayout.size() == 1);
            CHECK(missingLayout.front().PartUri == slide->Uri());
            CHECK_FALSE(result.IsValid());
        }

        SUBCASE("a master without a theme and a layout without its master")
        {
            auto master = presentation->AddSlideMasterPart();
            REQUIRE(master);
            auto layout = master->AddSlideLayoutPart();
            REQUIRE(layout);

            const auto result = ExyokiOffice::OpenXmlPackageValidator().Validate(*document);
            const auto missingTheme = IssuesWithId(result, ValidationErrorId::PackageSlideMasterMissingTheme);
            REQUIRE(missingTheme.size() == 1);
            CHECK(missingTheme.front().PartUri == master->Uri());

            const auto layoutWithoutMaster = IssuesWithId(result, ValidationErrorId::PackageSlideLayoutMissingSlideMaster);
            REQUIRE(layoutWithoutMaster.size() == 1);
            CHECK(layoutWithoutMaster.front().PartUri == layout->Uri());

            // The master part is related, but presentation.xml still does not
            // list it: PowerPoint reads the list, not the relationships.
            const auto unlisted = IssuesWithId(result, ValidationErrorId::PackagePresentationMissingSlideMaster);
            REQUIRE(unlisted.size() == 1);
            CHECK(unlisted.front().Message.find("no p:sldMasterIdLst entry for the related") != std::string::npos);
        }
    }

    TEST_CASE("S-5d: a deck with a listed master, a layout and a theme is accepted [opc][validation][semantic][powerpoint] [unit] [opc-validation]")
    {
        using namespace ExyokiOffice::PowerPoint;

        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor);
        auto master = editor->AddSlideMaster("Office");
        REQUIRE(master);
        auto layout = editor->AddSlideLayout(master, "Title and Content");
        REQUIRE(layout);
        REQUIRE(editor->AddSlide());
        REQUIRE(editor->SetSlideLayout(0, layout));

        const auto bytes = editor->SaveToMemory();
        REQUIRE_FALSE(bytes.empty());
        for (const bool withDom : {false, true})
        {
            CAPTURE(withDom);
            const auto result = ValidateSaved(bytes, withDom);
            CHECK(IssuesWithId(result, ValidationErrorId::PackagePresentationMissingSlideMaster).empty());
            CHECK(IssuesWithId(result, ValidationErrorId::PackageSlideMissingSlideLayout).empty());
            CHECK(IssuesWithId(result, ValidationErrorId::PackageSlideLayoutMissingSlideMaster).empty());
            CHECK(IssuesWithId(result, ValidationErrorId::PackageSlideMasterMissingTheme).empty());
            CHECK(IssuesWithId(result, ValidationErrorId::PackageDanglingRelationshipReference).empty());
        }
    }

    TEST_CASE("S-5e: a style reference nothing defines is a warning, not an error [opc][validation][semantic][word] [unit] [opc-validation]")
    {
        using namespace ExyokiOffice::Word;

        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor);
        auto first = editor->AddParagraph("Plain heading");
        REQUIRE(first);
        first->SetStyleId("Heading1");
        // A second reference to the same id is the same defect, reported once.
        auto second = editor->AddParagraph("Another heading");
        REQUIRE(second);
        second->SetStyleId("Heading1");

        SUBCASE("the W-1 shape: pStyle names a style styles.xml does not define")
        {
            const auto bytes = editor->SaveToMemory();
            REQUIRE_FALSE(bytes.empty());
            for (const bool withDom : {false, true})
            {
                CAPTURE(withDom);
                const auto result = ValidateSaved(bytes, withDom);
                const auto issues = IssuesWithId(result, ValidationErrorId::PackageStyleReferenceUndefined);
                REQUIRE(issues.size() == 1);
                CHECK(issues.front().Severity == ValidationSeverity::Warning);
                CHECK(issues.front().PartUri == "/word/document.xml");
                CHECK(issues.front().Message.find("Heading1") != std::string::npos);
                CHECK(issues.front().Location.ElementName.find("pStyle") != std::string::npos);
                // Word opens the file, so the package is still valid.
                CHECK_FALSE(result.HasErrors());
                CHECK(result.HasWarnings());
            }
        }

        SUBCASE("defining the style clears the warning")
        {
            StyleDefinition definition;
            definition.StyleId = "Heading1";
            definition.Name = "heading 1";
            definition.Type = StyleType::Paragraph;
            REQUIRE(editor->Styles().CreateStyle(definition));

            const auto result = ValidateSaved(editor->SaveToMemory(), false);
            CHECK(IssuesWithId(result, ValidationErrorId::PackageStyleReferenceUndefined).empty());
        }

        SUBCASE("run and table style references are checked the same way")
        {
            auto runs = first->Runs();
            REQUIRE_FALSE(runs.empty());
            runs.front()->SetStyleId("NoSuchCharacterStyle");

            const auto result = ValidateSaved(editor->SaveToMemory(), false);
            const auto issues = IssuesWithId(result, ValidationErrorId::PackageStyleReferenceUndefined);
            REQUIRE(issues.size() == 2);
            CHECK(std::any_of(issues.begin(), issues.end(), [](const ValidationIssue& issue)
                              { return issue.Message.find("NoSuchCharacterStyle") != std::string::npos &&
                                       issue.Location.ElementName.find("rStyle") != std::string::npos; }));
        }
    }

} // TEST_SUITE("PackageSemanticRuleTests")
