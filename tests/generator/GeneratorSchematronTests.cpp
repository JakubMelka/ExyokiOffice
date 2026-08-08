// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Spreadsheet.hpp"
#include "ExyokiOffice/DOM/OpenXmlElementFactory.hpp"
#include "ExyokiOffice/MetadataBuilder.hpp"
#include "Schematron.h"
#include "ExyokiOffice/StandardTypes.hpp"

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <utility>

using exyoki::generator::ClassifySchematronRule;
using exyoki::generator::LoadSchematronRules;
using exyoki::generator::SchematronPatternKind;
using ExyokiOffice::MetadataSchematronAttributeConstraint;

namespace
{
exyoki::generator::SchematronRule Classify(std::string test)
{
    return ClassifySchematronRule("x:test", std::move(test), "All", "schematrons.json");
}
} // namespace

TEST_SUITE("GeneratorSchematronTests")
{

    TEST_CASE("generated spreadsheet sheetData mappings use the worksheet schema type [unit] [generator]")
    {
        using ExyokiOffice::DocumentFormat::OpenXml::Spreadsheet::SheetData;
        const ExyokiOffice::OpenXmlQualifiedName name(
            "http://schemas.openxmlformats.org/spreadsheetml/2006/main", "sheetData");
        CHECK(ExyokiOffice::Generated::OpenXmlElementFactory::ResolveClass(name) ==
              SheetData::StaticMetaClass());

        const auto generatedPath = std::filesystem::path(EXYOKIOFFICE_SOURCE_DIR) /
                                   "sources/DOM/DocumentFormat/OpenXml/Spreadsheet.cpp";
        std::ifstream input(generatedPath, std::ios::binary);
        REQUIRE(input.good());
        const std::string generated((std::istreambuf_iterator<char>(input)),
                                    std::istreambuf_iterator<char>());
        const auto worksheet = generated.find("CreateWorksheetParticleMetadata()");
        REQUIRE(worksheet != std::string::npos);
        const auto nextClass = generated.find("class WorksheetMetaClass", worksheet);
        REQUIRE(nextClass != std::string::npos);
        const auto mapping = generated.find(
            "Spreadsheet::SheetData\", \"CT_SheetData/x:sheetData\"", worksheet);
        REQUIRE(mapping != std::string::npos);
        CHECK(mapping < nextClass);
        CHECK(generated.find("Spreadsheet::ExternalSheetData\", \"CT_SheetData/x:sheetData\"",
                             worksheet) == std::string::npos);
    }

    TEST_CASE("schematron classifier recognizes package relationship rules [unit] [generator] [generator-schematron]")
    {
        const auto rule = Classify(
            "document(rels)//r:Relationship[@Id = current()/@w:id]/@Type = "
            "'http://schemas.openxmlformats.org/officeDocument/2006/relationships/control'");

        CHECK(rule.kind == SchematronPatternKind::RelationshipType);
        REQUIRE(rule.operands.size() == 2);
        CHECK(rule.operands[0] == "w:id");
        CHECK(rule.operands[1] == "http://schemas.openxmlformats.org/officeDocument/2006/relationships/control");
    }

    TEST_CASE("schematron classifier recognizes cross-part reference rules [unit] [generator] [generator-schematron]")
    {
        const auto referenceRule =
            Classify("Index-of(document('Part:FootnotesPart')//w:footnotes/w:footnote/@w:id, @w:id)");
        CHECK(referenceRule.kind == SchematronPatternKind::PartReferenceExists);
        REQUIRE(referenceRule.operands.size() == 3);
        CHECK(referenceRule.operands[0] == "Part:FootnotesPart");
        CHECK(referenceRule.operands[1] == "w:footnotes/w:footnote/@w:id");
        CHECK(referenceRule.operands[2] == "w:id");

        const auto countRule =
            Classify("@x:s < count(document('Part:/WorkbookPart/WorkbookStylesPart')//x:cellXfs/x:xf) + 0");
        CHECK(countRule.kind == SchematronPatternKind::PartCountComparison);
        REQUIRE(countRule.operands.size() == 5);
        CHECK(countRule.operands[0] == "x:s");
        CHECK(countRule.operands[1] == "<");
        CHECK(countRule.operands[2] == "Part:/WorkbookPart/WorkbookStylesPart");
    }

    TEST_CASE("schematron classifier recognizes common element-local rules [unit] [generator] [generator-schematron]")
    {
        struct Case
        {
            const char* test;
            SchematronPatternKind kind;
        };

        constexpr std::array<Case, 12> cases{{
            {"count(distinct-values(//x:sheet/@x:sheetId)) = count(//x:sheet/@x:sheetId)",
             SchematronPatternKind::UniqueValues},
            {"count(distinct-values(ancestor::x:cellWatches//x:cellWatch/@x:r)) = "
             "count(ancestor::x:cellWatches//x:cellWatch/@x:r)",
             SchematronPatternKind::AncestorUniqueValues},
            {"matches(@x:name, \"[^'*\\[\\]/\\\\:?]{1}[^*\\[\\]/\\\\:?]*\")",
             SchematronPatternKind::AttributeRegex},
            {"string-length(@x:userName) >= 1 and string-length(@x:userName) <= 54",
             SchematronPatternKind::AttributeStringLength},
            {"@x:sheetId >= 1 and @x:sheetId <= 65534",
             SchematronPatternKind::AttributeNumericRange},
            {"@x:left >= 0 and @x:left < 49",
             SchematronPatternKind::AttributeNumericRange},
            {"@x:degree >= -1.7E308 and @x:degree <= 1.7E308",
             SchematronPatternKind::AttributeNumericRange},
            {"@w14:paraId > 0 and @w14:paraId < 0x80000000",
             SchematronPatternKind::AttributeNumericRange},
            {"@x:windowWidth <= 2147483647",
             SchematronPatternKind::AttributeNumericComparison},
            {"@x:guid != 00000000-0000-0000-0000-000000000000",
             SchematronPatternKind::AttributeInequality},
            {"@x:mode = auto or @x:mode = manual or @x:mode = none",
             SchematronPatternKind::AttributeAllowedValues},
            {"@x:totalsRowLabel and @x:totalsRowFunction = custom",
             SchematronPatternKind::AttributeImplication},
        }};

        for (const auto& item : cases)
        {
            CAPTURE(item.test);
            CHECK(Classify(item.test).kind == item.kind);
        }
    }

    TEST_CASE("schematron classifier extracts allowed attribute values [unit] [generator] [generator-schematron]")
    {
        const auto rule = Classify("@x:mode = auto or @x:mode = manual or @x:mode = none");

        CHECK(rule.kind == SchematronPatternKind::AttributeAllowedValues);
        REQUIRE(rule.operands.size() == 4);
        CHECK(rule.operands[0] == "x:mode");
        CHECK(rule.operands[1] == "auto");
        CHECK(rule.operands[2] == "manual");
        CHECK(rule.operands[3] == "none");
    }

    TEST_CASE("schematron classifier extracts attribute implication operands [unit] [generator] [generator-schematron]")
    {
        const auto equality = Classify("@x:totalsRowLabel and @x:totalsRowFunction = custom");
        CHECK(equality.kind == SchematronPatternKind::AttributeImplication);
        REQUIRE(equality.operands.size() == 4);
        CHECK(equality.operands[0] == "x:totalsRowLabel");
        CHECK(equality.operands[1] == "x:totalsRowFunction");
        CHECK(equality.operands[2] == "=");
        CHECK(equality.operands[3] == "custom");

        const auto inequality = Classify("@x14:aboveAverage and @x14:type != aboveAverage");
        CHECK(inequality.kind == SchematronPatternKind::AttributeImplication);
        REQUIRE(inequality.operands.size() == 4);
        CHECK(inequality.operands[0] == "x14:aboveAverage");
        CHECK(inequality.operands[1] == "x14:type");
        CHECK(inequality.operands[2] == "!=");
        CHECK(inequality.operands[3] == "aboveAverage");
    }

    TEST_CASE("schematron classifier extracts mutually exclusive attributes [unit] [generator] [generator-schematron]")
    {
        const auto rule = Classify(
            "(@x:auto and @x:indexed) or (@x:auto and @x:rgb) or (@x:auto and @x:theme) or "
            "(@x:indexed and @x:rgb) or (@x:indexed and @x:theme) or (@x:rgb and @x:theme)");

        CHECK(rule.kind == SchematronPatternKind::AttributeMutualExclusion);
        REQUIRE(rule.operands.size() == 4);
        CHECK(rule.operands[0] == "x:auto");
        CHECK(rule.operands[1] == "x:indexed");
        CHECK(rule.operands[2] == "x:rgb");
        CHECK(rule.operands[3] == "x:theme");
    }

    TEST_CASE("schematron classifier extracts remaining local boolean patterns [unit] [generator] [generator-schematron]")
    {
        const auto conditionalPresence = Classify("(@x:operator and @x:type = cells) or @x:type != cells");
        CHECK(conditionalPresence.kind == SchematronPatternKind::AttributeConditionalPresence);
        REQUIRE(conditionalPresence.operands.size() == 3);
        CHECK(conditionalPresence.operands[0] == "x:operator");
        CHECK(conditionalPresence.operands[1] == "x:type");
        CHECK(conditionalPresence.operands[2] == "cells");

        const auto conditionalRequiredValue = Classify("(@x:name = StdDocumentName and @x:ole = true) or @x:ole != true");
        CHECK(conditionalRequiredValue.kind == SchematronPatternKind::AttributeConditionalRequiredValue);
        REQUIRE(conditionalRequiredValue.operands.size() == 4);
        CHECK(conditionalRequiredValue.operands[0] == "x:name");
        CHECK(conditionalRequiredValue.operands[1] == "StdDocumentName");
        CHECK(conditionalRequiredValue.operands[2] == "x:ole");
        CHECK(conditionalRequiredValue.operands[3] == "true");

        // The imported rules drop the `not(...)` around the test, so a bare pair
        // states what must not happen together, exactly like the parenthesized
        // alternatives below. Reading it as "both required" rejects every
        // calculation-chain cell Excel writes.
        const auto exclusivePair = Classify("@x:dn and @x:r");
        CHECK(exclusivePair.kind == SchematronPatternKind::AttributeMutualExclusion);
        REQUIRE(exclusivePair.operands.size() == 2);
        CHECK(exclusivePair.operands[0] == "x:dn");
        CHECK(exclusivePair.operands[1] == "x:r");

        const auto forbiddenValues = Classify("@c:val != INF and @c:val != -INF and @c:val != NaN");
        CHECK(forbiddenValues.kind == SchematronPatternKind::AttributeForbiddenValues);
        REQUIRE(forbiddenValues.operands.size() == 4);
        CHECK(forbiddenValues.operands[0] == "c:val");
        CHECK(forbiddenValues.operands[1] == "INF");
        CHECK(forbiddenValues.operands[2] == "-INF");
        CHECK(forbiddenValues.operands[3] == "NaN");

        const auto attributeComparison = Classify("@x:min <= @x:max");
        CHECK(attributeComparison.kind == SchematronPatternKind::AttributeNumericAttributeComparison);
        REQUIRE(attributeComparison.operands.size() == 3);
        CHECK(attributeComparison.operands[0] == "x:min");
        CHECK(attributeComparison.operands[1] == "<=");
        CHECK(attributeComparison.operands[2] == "x:max");
    }

    TEST_CASE("schematron classifier extracts ancestor scoped unique value operands [unit] [generator] [generator-schematron]")
    {
        const auto rule =
            Classify("count(distinct-values(ancestor::x:table//x:tableColumn/@x:id)) = "
                     "count(ancestor::x:table//x:tableColumn/@x:id)");

        CHECK(rule.kind == SchematronPatternKind::AncestorUniqueValues);
        REQUIRE(rule.operands.size() == 2);
        CHECK(rule.operands[0] == "ancestor::x:table//x:tableColumn/@x:id");
        CHECK(rule.operands[1] == "ancestor::x:table//x:tableColumn/@x:id");
    }

    TEST_CASE("schematron classifier extracts mixed numeric range operands [unit] [generator] [generator-schematron]")
    {
        const auto rule = Classify("@w14:paraId > 0 and @w14:paraId < 0x80000000");

        CHECK(rule.kind == SchematronPatternKind::AttributeNumericRange);
        REQUIRE(rule.operands.size() == 5);
        CHECK(rule.operands[0] == "w14:paraId");
        CHECK(rule.operands[1] == ">");
        CHECK(rule.operands[2] == "0");
        CHECK(rule.operands[3] == "<");
        CHECK(rule.operands[4] == "0x80000000");
    }

    TEST_CASE("schematron classifier extracts conditional allowed values [unit] [generator] [generator-schematron]")
    {
        const auto rule = Classify(
            "((@x:type = none or @x:type = all) and (@x:scope = data or @x:scope = selection)) or "
            "(@x:scope != data and @x:scope != selection)");
        CHECK(rule.kind == SchematronPatternKind::AttributeConditionalAllowedValues);
        REQUIRE(rule.operands.size() == 8);
        CHECK(rule.operands[0] == "x:type");
        CHECK(rule.operands[1] == "2");
        CHECK(rule.operands[2] == "none");
        CHECK(rule.operands[3] == "all");
        CHECK(rule.operands[4] == "x:scope");
        CHECK(rule.operands[5] == "2");
        CHECK(rule.operands[6] == "data");
        CHECK(rule.operands[7] == "selection");
    }

    TEST_CASE("schematron classifier preserves trigger attribute for conditional presence allowed values [unit] [generator] [generator-schematron]")
    {
        const auto rule = Classify("@x14:dxfId and (@x14:sortBy = icon or @x14:sortBy = value)");
        CHECK(rule.kind == SchematronPatternKind::AttributeConditionalPresenceAllowedValues);
        REQUIRE(rule.operands.size() == 4);
        CHECK(rule.operands[0] == "x14:dxfId");
        CHECK(rule.operands[1] == "x14:sortBy");
        CHECK(rule.operands[2] == "icon");
        CHECK(rule.operands[3] == "value");
    }

    TEST_CASE("current schematron metadata is fully covered by supported patterns [unit] [generator] [generator-schematron]")
    {
        const auto dataRoot = std::filesystem::path(EXYOKIOFFICE_SOURCE_DIR) / "data" / "schematrons.json";
        const auto rules = LoadSchematronRules(dataRoot);
        REQUIRE(rules.size() == 948);

        std::map<SchematronPatternKind, ExyokiOffice::Size> counts;
        for (const auto& rule : rules)
        {
            ++counts[rule.kind];
        }

        CHECK(counts[SchematronPatternKind::Unsupported] == 0);
        CHECK(counts[SchematronPatternKind::RelationshipType] == 27);
        CHECK(counts[SchematronPatternKind::PartReferenceExists] == 23);
        CHECK(counts[SchematronPatternKind::PartCountComparison] == 53);
        CHECK(counts[SchematronPatternKind::AttributeRegex] == 17);
        CHECK(counts[SchematronPatternKind::AttributeStringLength] == 191);
        CHECK(counts[SchematronPatternKind::AttributeNumericRange] >= 100);
        CHECK(counts[SchematronPatternKind::AttributeNumericComparison] >= 100);
        CHECK(counts[SchematronPatternKind::AttributeAllowedValues] == 37);
        CHECK(counts[SchematronPatternKind::AncestorUniqueValues] == 25);
        CHECK(counts[SchematronPatternKind::AttributeConditionalAllowedValues] > 0);
        CHECK(counts[SchematronPatternKind::AttributeConditionalPresenceAllowedValues] > 0);
    }

    TEST_CASE("generated metadata contains element-local schematron constraints [unit] [generator] [generator-schematron]")
    {
        using ExyokiOffice::DocumentFormat::OpenXml::Spreadsheet::CustomWorkbookView;

        const auto metadata = CustomWorkbookView::StaticMetaClass()->GetMetadata();
        REQUIRE(metadata != nullptr);

        ExyokiOffice::Size schematronConstraintCount = 0;
        for (const auto& constraint : metadata->Constraints())
        {
            if (std::dynamic_pointer_cast<MetadataSchematronAttributeConstraint>(constraint))
            {
                ++schematronConstraintCount;
            }
        }

        CHECK(schematronConstraintCount == 5);
    }

} // TEST_SUITE("GeneratorSchematronTests")
