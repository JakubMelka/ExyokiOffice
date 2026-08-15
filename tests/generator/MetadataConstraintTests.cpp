// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/MetadataBuilder.hpp"
#include "ExyokiOffice/OpenXmlDomValidator.hpp"
#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/OpenXmlPackagePart.hpp"
#include "ExyokiOffice/OpenXmlPackageValidator.hpp"
#include "ExyokiOffice/OpenXmlSimpleTypes.hpp"
#include "ExyokiOffice/OpenXMLElement.hpp"
#include "ExyokiOffice/XmlLocationCache.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Presentation.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Spreadsheet.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Wordprocessing.hpp"
#include "zip/zip.h"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{

constexpr std::string_view kTestNs = "urn:exyokioffice:test";
constexpr std::string_view kWordNs = "http://schemas.openxmlformats.org/wordprocessingml/2006/main";

ExyokiOffice::OpenXmlQualifiedName Attr(std::string_view localName)
{
    return ExyokiOffice::OpenXmlQualifiedName(kTestNs, localName);
}

void AddZipEntry(zip_t* archive, const char* name, std::string_view content)
{
    REQUIRE(zip_entry_open(archive, name) == 0);
    CHECK(zip_entry_write(archive, content.data(), content.size()) == 0);
    zip_entry_close(archive);
}

std::vector<ExyokiOffice::Byte> FinishZip(zip_t* archive)
{
    void* rawBuffer = nullptr;
    ExyokiOffice::Size rawSize = 0;
    REQUIRE(zip_stream_copy(archive, &rawBuffer, &rawSize) > 0);
    zip_stream_close(archive);
    REQUIRE(rawBuffer != nullptr);

    const auto* bytes = static_cast<const ExyokiOffice::UInt8*>(rawBuffer);
    std::vector<ExyokiOffice::Byte> result(bytes, bytes + rawSize);
    std::free(rawBuffer);
    return result;
}

std::vector<ExyokiOffice::Byte> BuildSingleXmlPartPackage(std::string_view xml)
{
    auto* archive = zip_stream_open(nullptr, 0, ZIP_DEFAULT_COMPRESSION_LEVEL, 'w');
    REQUIRE(archive != nullptr);

    AddZipEntry(archive,
                "[Content_Types].xml",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="xml" ContentType="application/xml"/>
</Types>)");
    AddZipEntry(archive, "custom.xml", xml);
    return FinishZip(archive);
}

std::shared_ptr<ExyokiOffice::OpenXMLElement> LoadRootElement(std::string_view xml)
{
    static std::vector<std::shared_ptr<ExyokiOffice::OpenXmlPackage>> packages;

    auto package = std::make_shared<ExyokiOffice::OpenXmlPackage>();
    REQUIRE(package->LoadFromMemory(BuildSingleXmlPartPackage(xml)));

    auto part = package->GetPartByUri("/custom.xml");
    REQUIRE(part != nullptr);

    auto root = part->GetRootElement();
    REQUIRE(root != nullptr);
    packages.push_back(std::move(package));
    return root;
}

std::shared_ptr<ExyokiOffice::OpenXMLElement> MakeAttributeElement(std::string_view attributes)
{
    std::string xml = R"(<?xml version="1.0" encoding="UTF-8"?>)";
    xml += R"(<t:root xmlns:t=")";
    xml += kTestNs;
    xml += R"(" )";
    xml += attributes;
    xml += "/>";
    return LoadRootElement(xml);
}

std::shared_ptr<ExyokiOffice::OpenXMLElement> MakeWordTextElement(std::string_view text)
{
    std::string xml = R"(<?xml version="1.0" encoding="UTF-8"?>)";
    xml += R"(<w:t xmlns:w=")";
    xml += kWordNs;
    xml += R"(">)";
    xml += text;
    xml += "</w:t>";
    return LoadRootElement(xml);
}

bool IsValid(const ExyokiOffice::MetadataConstraint& constraint, const ExyokiOffice::OpenXMLElement& element)
{
    ExyokiOffice::XmlLocationCache locations;
    return constraint.Validate(element, locations).IsValid();
}

ExyokiOffice::ValidationIssue SingleIssue(const ExyokiOffice::MetadataConstraint& constraint,
                                          const ExyokiOffice::OpenXMLElement& element)
{
    ExyokiOffice::XmlLocationCache locations;
    const auto result = constraint.Validate(element, locations);
    REQUIRE(result.Issues().size() == 1);
    return result.Issues().front();
}

bool HasIssue(const ExyokiOffice::MetadataConstraint& constraint,
              const ExyokiOffice::OpenXMLElement& element,
              ExyokiOffice::ValidationErrorId id)
{
    ExyokiOffice::XmlLocationCache locations;
    const auto result = constraint.Validate(element, locations);
    for (const auto& issue : result.Issues())
    {
        if (issue.Id == id)
        {
            return true;
        }
    }
    return false;
}

class TestParticleMetaClass final : public ExyokiOffice::OpenXMLElementClass
{
public:
    explicit TestParticleMetaClass(ExyokiOffice::MetadataParticlePtr particle)
        : particle_(std::move(particle))
    {
    }

    const ExyokiOffice::OpenXMLElementClass* GetBaseMetaClass() const noexcept override
    {
        return ExyokiOffice::OpenXmlCompositeElement::StaticMetaClass();
    }
    ExyokiOffice::OpenXmlQualifiedName QualifiedName() const noexcept override
    {
        return {kTestNs, "root"};
    }
    ExyokiOffice::OpenXmlQualifiedName TypeQualifiedName() const noexcept override
    {
        return {kTestNs, "RootType"};
    }
    ExyokiOffice::OpenXml::FileFormatVersions GetVersion() const noexcept override
    {
        return ExyokiOffice::OpenXml::FileFormatVersions::Office2007;
    }
    std::shared_ptr<ExyokiOffice::OpenXMLElement> Create() const override { return nullptr; }

protected:
    void ConfigureMetadata(ExyokiOffice::MetadataBuilder& builder) const override
    {
        builder.SetParticleTree(particle_);
    }

private:
    ExyokiOffice::MetadataParticlePtr particle_;
};

class TestParticleElement final : public ExyokiOffice::OpenXmlCompositeElement
{
public:
    TestParticleElement(const ExyokiOffice::OpenXMLElementClass* metaClass,
                        const std::shared_ptr<ExyokiOffice::OpenXMLElement>& source)
        : metaClass_(metaClass)
    {
        SetNodeHandle(source->GetNodeHandle());
    }

    const ExyokiOffice::OpenXMLElementClass* ElementMetaClass() const noexcept override
    {
        return metaClass_;
    }

private:
    const ExyokiOffice::OpenXMLElementClass* metaClass_;
};

class TestConstraintMetaClass final : public ExyokiOffice::OpenXMLElementClass
{
public:
    const ExyokiOffice::OpenXMLElementClass* GetBaseMetaClass() const noexcept override
    {
        return ExyokiOffice::OpenXmlLeafTextElement::StaticMetaClass();
    }
    ExyokiOffice::OpenXmlQualifiedName QualifiedName() const noexcept override { return {kTestNs, "root"}; }
    ExyokiOffice::OpenXmlQualifiedName TypeQualifiedName() const noexcept override { return {kTestNs, "RootType"}; }
    ExyokiOffice::OpenXml::FileFormatVersions GetVersion() const noexcept override
    {
        return ExyokiOffice::OpenXml::FileFormatVersions::Office2007;
    }
    std::shared_ptr<ExyokiOffice::OpenXMLElement> Create() const override { return nullptr; }

protected:
    void ConfigureMetadata(ExyokiOffice::MetadataBuilder& builder) const override
    {
        auto& code = builder.AddAttribute(Attr("code"), "Code", "StringValue",
                                          ExyokiOffice::OpenXml::FileFormatVersions::Office2007, {});
        code.Validators.push_back(
            std::make_shared<ExyokiOffice::MetadataRequiredConstraint>(Attr("code"), "Code", true));
        code.Validators.push_back(std::make_shared<ExyokiOffice::MetadataStringConstraint>(
            Attr("code"), "Code", std::nullopt, std::nullopt, 3, std::string("[A-Z]{3}"),
            true, false, false, false, false));

        auto& count = builder.AddAttribute(Attr("count"), "Count", "Int32Value",
                                           ExyokiOffice::OpenXml::FileFormatVersions::Office2007, {});
        count.Validators.push_back(std::make_shared<ExyokiOffice::MetadataNumberConstraint>(
            Attr("count"), "Count", "Int32Value", 1.0, 10.0, std::nullopt, std::nullopt,
            false, false, false));

        auto& kind = builder.AddAttribute(Attr("kind"), "Kind", "EnumValue",
                                          ExyokiOffice::OpenXml::FileFormatVersions::Office2007, {});
        kind.Validators.push_back(std::make_shared<ExyokiOffice::MetadataAttributeEnumUnionConstraint>(
            Attr("kind"), "Kind",
            std::vector<ExyokiOffice::MetadataEnumRule>{{{"alpha", "beta"},
                                                         ExyokiOffice::OpenXml::FileFormatVersions::Office2007,
                                                         0,
                                                         true}}));

        auto& future = builder.AddAttribute(Attr("future"), "Future", "StringValue",
                                            ExyokiOffice::OpenXml::FileFormatVersions::Office2010, {});
        future.Validators.push_back(std::make_shared<ExyokiOffice::MetadataOfficeVersionConstraint>(
            Attr("future"), "Future", ExyokiOffice::OpenXml::FileFormatVersions::Office2010));

        builder.AddConstraint(std::make_shared<ExyokiOffice::MetadataTextNumberConstraint>(
            "DoubleValue", 0.0, 100.0, std::nullopt, std::nullopt, false, true, false));
    }
};

class TestConstraintElement final : public ExyokiOffice::OpenXmlLeafTextElement
{
public:
    TestConstraintElement(const ExyokiOffice::OpenXMLElementClass* metaClass,
                          const std::shared_ptr<ExyokiOffice::OpenXMLElement>& source)
        : metaClass_(metaClass)
    {
        SetNodeHandle(source->GetNodeHandle());
    }

    const ExyokiOffice::OpenXMLElementClass* ElementMetaClass() const noexcept override
    {
        return metaClass_;
    }

private:
    const ExyokiOffice::OpenXMLElementClass* metaClass_;
};

ExyokiOffice::MetadataParticlePtr ElementParticle(std::string_view localName,
                                                  ExyokiOffice::UInt32 minOccurs = 1,
                                                  std::optional<ExyokiOffice::UInt32> maxOccurs = 1)
{
    return std::make_shared<ExyokiOffice::MetadataElementParticle>(
        ExyokiOffice::OpenXmlQualifiedName(kTestNs, localName),
        std::string(localName),
        std::string(localName),
        minOccurs,
        maxOccurs,
        ExyokiOffice::OpenXml::FileFormatVersions::Office2007);
}

bool ValidateParticleXml(std::string_view children, ExyokiOffice::MetadataParticlePtr particle)
{
    std::string xml = R"(<t:root xmlns:t="urn:exyokioffice:test">)";
    xml += children;
    xml += "</t:root>";
    const auto source = LoadRootElement(xml);
    TestParticleMetaClass metaClass(std::move(particle));
    TestParticleElement element(&metaClass, source);
    return ExyokiOffice::OpenXmlDomValidator().ValidateElement(element).IsValid();
}

bool HasValidationIssue(const ExyokiOffice::ValidationResult& result,
                        ExyokiOffice::ValidationErrorId id)
{
    return std::any_of(result.Issues().begin(), result.Issues().end(),
                       [id](const auto& issue)
                       { return issue.Id == id; });
}

bool ContainsUnboundedParticle(const ExyokiOffice::MetadataParticlePtr& particle)
{
    if (!particle)
    {
        return false;
    }
    if (particle->IsUnbounded())
    {
        return true;
    }
    const auto composite =
        std::dynamic_pointer_cast<ExyokiOffice::MetadataCompositeParticle>(particle);
    if (!composite)
    {
        return false;
    }
    return std::any_of(composite->Children().begin(), composite->Children().end(),
                       [](const auto& child)
                       { return ContainsUnboundedParticle(child); });
}

ExyokiOffice::ValidationResult ValidateConstraintXml(std::string_view attributes,
                                                     std::string_view text)
{
    std::string xml = R"(<t:root xmlns:t="urn:exyokioffice:test" )";
    xml += attributes;
    xml += ">";
    xml += text;
    xml += "</t:root>";
    const auto source = LoadRootElement(xml);
    TestConstraintMetaClass metaClass;
    TestConstraintElement element(&metaClass, source);
    return ExyokiOffice::OpenXmlDomValidator().ValidateElement(element);
}

} // namespace

TEST_SUITE("MetadataConstraintTests")
{

    TEST_CASE("metaclass metadata is cached and reused across threads [unit] [metadata] [metadata-cache]")
    {
        using ExyokiOffice::DocumentFormat::OpenXml::Spreadsheet::Worksheet;
        const auto* metaClass = Worksheet::StaticMetaClass();
        const auto expected = metaClass->GetMetadata();
        std::array<std::shared_ptr<ExyokiOffice::MetadataDefinition>, 8> results;
        std::array<std::thread, 8> threads;
        for (ExyokiOffice::Size index = 0; index < threads.size(); ++index)
        {
            threads[index] = std::thread([&, index]()
                                         { results[index] = metaClass->GetMetadata(); });
        }
        for (auto& thread : threads)
        {
            thread.join();
        }
        for (const auto& result : results)
        {
            CHECK(result == expected);
        }
    }

    TEST_CASE("generated Word repeating content preserves unbounded particle metadata [unit] [metadata] [generator]")
    {
        using ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Body;
        const auto metadata = Body::StaticMetaClass()->GetMetadata();
        REQUIRE(metadata);
        CHECK(ContainsUnboundedParticle(metadata->ParticleTree()));
    }

    TEST_CASE("DOM validator executes generated attribute validators [unit] [metadata] [dom-validation]")
    {
        auto element = LoadRootElement(
            R"(<x:oleSize xmlns:x="http://schemas.openxmlformats.org/spreadsheetml/2006/main"/>)");
        const auto result = ExyokiOffice::OpenXmlDomValidator().ValidateElement(*element);
        REQUIRE_FALSE(result.IsValid());
        REQUIRE(result.Issues().size() == 1);
        CHECK(result.Issues().front().Id == ExyokiOffice::ValidationErrorId::MissingAttribute);
        CHECK(result.Issues().front().Domain == ExyokiOffice::ValidationDomain::Schema);
    }

    TEST_CASE("DOM validator aggregates required string number enum and text metadata [unit] [metadata] [dom-validation]")
    {
        const auto valid = ValidateConstraintXml(R"(t:code="ABC" t:count="5" t:kind="alpha")", "42");
        CHECK(valid.IsValid());

        const auto invalid = ValidateConstraintXml(R"(t:code="x" t:count="oops" t:kind="gamma")", "-1");
        CHECK(HasValidationIssue(invalid, ExyokiOffice::ValidationErrorId::AttributeExactLengthMismatch));
        CHECK(HasValidationIssue(invalid, ExyokiOffice::ValidationErrorId::AttributePatternMismatch));
        CHECK(HasValidationIssue(invalid, ExyokiOffice::ValidationErrorId::AttributeNumberParsingFailed));
        CHECK(HasValidationIssue(invalid, ExyokiOffice::ValidationErrorId::AttributeEnumViolation));
        CHECK(HasValidationIssue(invalid, ExyokiOffice::ValidationErrorId::TextMinInclusiveViolation));
        CHECK(invalid.Issues().size() >= 5);
        for (const auto& issue : invalid.Issues())
        {
            CHECK(issue.Domain == ExyokiOffice::ValidationDomain::Schema);
            CHECK(issue.Location.IsValid());
        }

        const auto missing = ValidateConstraintXml(R"(t:count="11" t:kind="beta")", "101");
        CHECK(HasValidationIssue(missing, ExyokiOffice::ValidationErrorId::MissingAttribute));
        CHECK(HasValidationIssue(missing, ExyokiOffice::ValidationErrorId::AttributeMaxInclusiveViolation));
        CHECK(HasValidationIssue(missing, ExyokiOffice::ValidationErrorId::TextMaxInclusiveViolation));
    }

    TEST_CASE("DOM validator applies Office target version only to present versioned attributes [unit] [metadata] [dom-validation]")
    {
        const auto sourceWithoutFuture = LoadRootElement(
            R"(<t:root xmlns:t="urn:exyokioffice:test" t:code="ABC" t:count="5" t:kind="alpha">42</t:root>)");
        const auto sourceWithFuture = LoadRootElement(
            R"(<t:root xmlns:t="urn:exyokioffice:test" t:code="ABC" t:count="5" t:kind="alpha" t:future="yes">42</t:root>)");
        TestConstraintMetaClass metaClass;
        TestConstraintElement withoutFuture(&metaClass, sourceWithoutFuture);
        TestConstraintElement withFuture(&metaClass, sourceWithFuture);

        const ExyokiOffice::OpenXmlDomValidator office2007(
            {ExyokiOffice::OpenXml::FileFormatVersions::Office2007});
        CHECK(office2007.ValidateElement(withoutFuture).IsValid());
        CHECK(HasValidationIssue(office2007.ValidateElement(withFuture),
                                 ExyokiOffice::ValidationErrorId::AttributeVersionViolation));

        const ExyokiOffice::OpenXmlDomValidator office2010(
            {ExyokiOffice::OpenXml::FileFormatVersions::Office2010});
        CHECK(office2010.ValidateElement(withFuture).IsValid());
        CHECK(ExyokiOffice::OpenXmlDomValidator().ValidateElement(withFuture).IsValid());
    }

    TEST_CASE("DOM particle validator supports sequence choice all group any and cardinality [unit] [metadata] [dom-validation]")
    {
        using namespace ExyokiOffice;
        const auto version = OpenXml::FileFormatVersions::Office2007;

        auto sequence = std::make_shared<MetadataSequenceParticle>(1, 1, version);
        sequence->AddChild(ElementParticle("a"));
        sequence->AddChild(ElementParticle("b"));
        CHECK(ValidateParticleXml("<t:a/><t:b/>", sequence));
        CHECK_FALSE(ValidateParticleXml("<t:b/><t:a/>", sequence));

        auto choice = std::make_shared<MetadataChoiceParticle>(1, 1, version);
        choice->AddChild(ElementParticle("a"));
        choice->AddChild(ElementParticle("b"));
        CHECK(ValidateParticleXml("<t:b/>", choice));
        CHECK_FALSE(ValidateParticleXml("<t:c/>", choice));

        auto nullableChoice = std::make_shared<MetadataChoiceParticle>(1, 1, version);
        nullableChoice->AddChild(ElementParticle("a", 0, 1));
        CHECK(ValidateParticleXml("", nullableChoice));

        auto all = std::make_shared<MetadataAllParticle>(1, 1, version);
        all->AddChild(ElementParticle("a"));
        all->AddChild(ElementParticle("b"));
        CHECK(ValidateParticleXml("<t:b/><t:a/>", all));
        CHECK_FALSE(ValidateParticleXml("<t:a/><t:a/><t:b/>", all));

        auto optionalAll = std::make_shared<MetadataAllParticle>(1, 1, version);
        optionalAll->AddChild(ElementParticle("a", 0, 1));
        optionalAll->AddChild(ElementParticle("b"));
        CHECK(ValidateParticleXml("<t:b/>", optionalAll));
        CHECK(ValidateParticleXml("<t:b/><t:a/>", optionalAll));

        auto group = std::make_shared<MetadataGroupParticle>(1, 1, version);
        group->AddChild(sequence);
        CHECK(ValidateParticleXml("<t:a/><t:b/>", group));

        auto any = std::make_shared<MetadataAnyParticle>("##any", 1, 1, version);
        CHECK(ValidateParticleXml("<t:unknown/>", any));

        auto localAny = std::make_shared<MetadataAnyParticle>("##local", 1, 1, version);
        CHECK(ValidateParticleXml("<unknown/>", localAny));
        CHECK_FALSE(ValidateParticleXml("<t:unknown/>", localAny));

        auto otherAny = std::make_shared<MetadataAnyParticle>("##other", 1, 1, version);
        CHECK(ValidateParticleXml("<o:unknown xmlns:o=\"urn:other\"/>", otherAny));
        CHECK_FALSE(ValidateParticleXml("<t:unknown/>", otherAny));

        auto namespaceAny = std::make_shared<MetadataAnyParticle>("urn:other ##local", 1, 1, version);
        CHECK(ValidateParticleXml("<o:unknown xmlns:o=\"urn:other\"/>", namespaceAny));
        CHECK(ValidateParticleXml("<unknown/>", namespaceAny));

        CHECK(ValidateParticleXml("<t:a/><t:a/>", ElementParticle("a", 2, 2)));
        CHECK_FALSE(ValidateParticleXml("<t:a/>", ElementParticle("a", 2, 2)));
        CHECK_FALSE(ValidateParticleXml("<t:a/><t:a/><t:a/>", ElementParticle("a", 2, 2)));
        CHECK(ValidateParticleXml("", ElementParticle("a", 0, std::nullopt)));
        CHECK(ValidateParticleXml("<t:a/><t:a/><t:a/><t:a/>", ElementParticle("a", 0, std::nullopt)));

        auto nested = std::make_shared<MetadataSequenceParticle>(1, 1, version);
        auto repeatedChoice = std::make_shared<MetadataChoiceParticle>(1, std::nullopt, version);
        repeatedChoice->AddChild(ElementParticle("a"));
        repeatedChoice->AddChild(ElementParticle("b"));
        nested->AddChild(repeatedChoice);
        nested->AddChild(ElementParticle("c"));
        CHECK(ValidateParticleXml("<t:a/><t:b/><t:a/><t:c/>", nested));
        CHECK_FALSE(ValidateParticleXml("<t:a/><t:c/><t:b/>", nested));
    }

    TEST_CASE("real Word Excel and PowerPoint content models accept valid order and reject invalid order [unit] [metadata] [dom-validation]")
    {
        const ExyokiOffice::OpenXmlDomValidator validator;

        auto wordValid = LoadRootElement(
            R"(<w:body xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:p/><w:p/><w:sectPr/></w:body>)");
        auto wordInvalid = LoadRootElement(
            R"(<w:body xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:sectPr/><w:p/></w:body>)");
        CHECK(validator.ValidateElement(*wordValid).IsValid());
        CHECK(HasValidationIssue(validator.ValidateElement(*wordInvalid),
                                 ExyokiOffice::ValidationErrorId::ParticleConstraintViolation));

        auto excelValid = LoadRootElement(
            R"(<x:workbook xmlns:x="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><x:workbookPr/><x:sheets/></x:workbook>)");
        auto excelInvalid = LoadRootElement(
            R"(<x:workbook xmlns:x="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><x:sheets/><x:workbookPr/></x:workbook>)");
        CHECK(validator.ValidateElement(*excelValid).IsValid());
        CHECK(HasValidationIssue(validator.ValidateElement(*excelInvalid),
                                 ExyokiOffice::ValidationErrorId::ParticleConstraintViolation));

        auto powerPointValid = LoadRootElement(
            R"(<p:presentation xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main"><p:sldSz/><p:notesSz/></p:presentation>)");
        auto powerPointInvalid = LoadRootElement(
            R"(<p:presentation xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main"><p:notesSz/><p:sldSz/></p:presentation>)");
        CHECK(validator.ValidateElement(*powerPointValid).IsValid());
        CHECK(HasValidationIssue(validator.ValidateElement(*powerPointInvalid),
                                 ExyokiOffice::ValidationErrorId::ParticleConstraintViolation));
    }

    TEST_CASE("an element name shared by several types is validated as the one the parent declares [unit] [metadata] [dom-validation]")
    {
        const ExyokiOffice::OpenXmlDomValidator validator;

        // `w:bottom` is a border inside `w:pBdr` and a width inside
        // `w:tblCellMar`. The width rules (`@w:w`, `@w:type`) must not be
        // applied to the border, and must still be applied to the width.
        auto borders = LoadRootElement(
            R"(<w:pBdr xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">)"
            R"(<w:bottom w:val="single" w:sz="8" w:space="0" w:color="1F4E79"/></w:pBdr>)");
        const auto borderResult = validator.Validate(*borders);
        CHECK(borderResult.IsValid());
        CHECK_FALSE(HasValidationIssue(borderResult, ExyokiOffice::ValidationErrorId::MissingAttribute));

        auto margins = LoadRootElement(
            R"(<w:tblCellMar xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">)"
            R"(<w:bottom/></w:tblCellMar>)");
        CHECK(HasValidationIssue(validator.Validate(*margins),
                                 ExyokiOffice::ValidationErrorId::MissingAttribute));

        auto completeMargins = LoadRootElement(
            R"(<w:tblCellMar xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">)"
            R"(<w:bottom w:w="120" w:type="dxa"/></w:tblCellMar>)");
        CHECK(validator.Validate(*completeMargins).IsValid());
    }

    TEST_CASE("a schematron attribute is checked under the name its element declares [unit] [metadata] [dom-validation]")
    {
        // The schematron source spells the hyperlink's relationship attribute
        // with the element's own prefix (`@a:id`); what `a:hlinkClick` really
        // carries is `r:id`.
        const ExyokiOffice::OpenXmlDomValidator validator;

        auto linked = LoadRootElement(
            R"(<a:hlinkClick xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" )"
            R"(xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships" r:id="rId2"/>)");
        CHECK(validator.ValidateElement(*linked).IsValid());

        auto unlinked = LoadRootElement(
            R"(<a:hlinkClick xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"/>)");
        CHECK(HasValidationIssue(validator.ValidateElement(*unlinked),
                                 ExyokiOffice::ValidationErrorId::MissingAttribute));
    }

    TEST_CASE("recursive and package validation report descendant schema errors with part URI [unit] [metadata] [dom-validation]")
    {
        const auto bytes = BuildSingleXmlPartPackage(
            R"(<x:workbook xmlns:x="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><x:oleSize/></x:workbook>)");
        ExyokiOffice::OpenXmlPackage package;
        REQUIRE(package.LoadFromMemory(bytes));
        auto part = package.GetPartByUri("/custom.xml");
        REQUIRE(part);
        auto root = part->GetRootElement();
        REQUIRE(root);

        const auto domResult = ExyokiOffice::OpenXmlDomValidator().Validate(*root);
        CHECK(HasValidationIssue(domResult, ExyokiOffice::ValidationErrorId::MissingAttribute));

        const auto packageResult = ExyokiOffice::OpenXmlPackageValidator(
                                       ExyokiOffice::OpenXmlDomValidationSettings{})
                                       .Validate(package);
        const auto issue = std::find_if(packageResult.Issues().begin(), packageResult.Issues().end(),
                                        [](const auto& candidate)
                                        {
                                            return candidate.Id == ExyokiOffice::ValidationErrorId::MissingAttribute;
                                        });
        REQUIRE(issue != packageResult.Issues().end());
        CHECK(issue->Domain == ExyokiOffice::ValidationDomain::Schema);
        CHECK(issue->PartUri == "/custom.xml");
        CHECK(issue->Location.IsValid());

        // The diagnostic addresses the offending element and attribute, not just
        // the part it happens to live in.
        CHECK(issue->Location.Path == "/x:workbook/x:oleSize");
        CHECK(issue->Location.ElementName == "x:oleSize");
        CHECK(issue->Location.AttributeName == "ref");
        CHECK(issue->Message.find("'ref'") != std::string::npos);
    }

    TEST_CASE("particle violations name the offending child and what was expected [unit] [metadata] [dom-validation]")
    {
        const ExyokiOffice::OpenXmlDomValidator validator;
        const auto particleIssue = [&](std::string_view xml)
        {
            const auto result = validator.ValidateElement(*LoadRootElement(xml));
            const auto issue =
                std::find_if(result.Issues().begin(), result.Issues().end(), [](const auto& candidate)
                             { return candidate.Id ==
                                      ExyokiOffice::ValidationErrorId::ParticleConstraintViolation; });
            REQUIRE(issue != result.Issues().end());
            return *issue;
        };

        SUBCASE("a child in the wrong place is blamed by index, name and location")
        {
            const auto issue = particleIssue(
                R"(<w:body xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">)"
                R"(<w:sectPr/><w:p/></w:body>)");

            CHECK(issue.Message.find("Content of element 'w:body'") != std::string::npos);
            CHECK(issue.Message.find("child 2 is 'w:p'") != std::string::npos);

            // The diagnostic points at the child that broke the model, not at the
            // parent whose content model it broke.
            CHECK(issue.Location.Path == "/w:body/w:p");
            CHECK(issue.Location.ElementName == "w:p");
        }

        // There is deliberately no case for content that ends before a required
        // particle is satisfied. Every particle tree in the imported metadata is
        // itself declared with minOccurs 0, so an empty or truncated content model
        // always matches by taking zero occurrences; the reporting path for it
        // exists, but no document can currently reach it.

        SUBCASE("a child appearing too often is blamed at the repeat")
        {
            const auto issue = particleIssue(
                R"(<w:tbl xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">)"
                R"(<w:tblPr/><w:tblPr/></w:tbl>)");

            CHECK(issue.Message.find("child 2 is 'w:tblPr'") != std::string::npos);
            CHECK(issue.Location.Path == "/w:tbl/w:tblPr[2]");
        }

        SUBCASE("a child from the wrong namespace lists the names the model wanted")
        {
            // Same local name, wrong namespace: a:cNvPr where the PresentationML
            // content model wants p:cNvPr. Without the expected-name list this is
            // very hard to see, because both spell out as "cNvPr" when read.
            const auto issue = particleIssue(
                R"(<p:nvSpPr xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main")"
                R"( xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main">)"
                R"(<a:cNvPr/></p:nvSpPr>)");

            CHECK(issue.Message.find("child 1 is 'a:cNvPr'") != std::string::npos);
            CHECK(issue.Message.find("expected one of 'p:cNvPr'") != std::string::npos);
        }
    }

    TEST_CASE("an empty required attribute is judged by its declared type [unit] [metadata] [dom-validation]")
    {
        const ExyokiOffice::OpenXmlDomValidator validator;
        const auto validate = [&](std::string_view xml)
        {
            return validator.ValidateElement(*LoadRootElement(xml));
        };

        // p:cNvPr/@name is xsd:string and required. Required means present, and the
        // empty string is a perfectly good string - PowerPoint itself writes
        // name="" on the root group of every shape tree.
        const auto emptyName = validate(
            R"(<p:cNvPr xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main" id="1" name=""/>)");
        CHECK_FALSE(HasValidationIssue(emptyName, ExyokiOffice::ValidationErrorId::EmptyAttribute));
        CHECK(emptyName.IsValid());

        // @id is UInt32Value: an empty value parses into nothing, so it really is
        // an attribute without a value.
        const auto emptyId = validate(
            R"(<p:cNvPr xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main" id="" name="Shape"/>)");
        CHECK(HasValidationIssue(emptyId, ExyokiOffice::ValidationErrorId::EmptyAttribute));

        // A missing required attribute stays an error whatever its type is.
        const auto missing = validate(
            R"(<p:cNvPr xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main" id="1"/>)");
        CHECK(HasValidationIssue(missing, ExyokiOffice::ValidationErrorId::MissingAttribute));

        // x:dataValidation/@sqref is a required ListValue<StringValue>. An xsd:list
        // spells zero items as the empty string, and where the schema really wants
        // content it says so with its own string-length rule, so emptiness alone is
        // not what makes this attribute wrong.
        const auto emptySqref = validate(
            R"(<x:dataValidation xmlns:x="http://schemas.openxmlformats.org/spreadsheetml/2006/main" sqref=""/>)");
        CHECK_FALSE(HasValidationIssue(emptySqref, ExyokiOffice::ValidationErrorId::EmptyAttribute));

        // v:shape/@o:ole is ST_TrueFalseBlank, the one boolean family whose lexical
        // space contains the blank. The simple type parses it, so the validator must
        // not contradict the parser by calling it valueless.
        const auto emptyBlank = validate(
            R"(<v:shape xmlns:v="urn:schemas-microsoft-com:vml")"
            R"( xmlns:o="urn:schemas-microsoft-com:office:office" o:ole=""/>)");
        CHECK_FALSE(HasValidationIssue(emptyBlank, ExyokiOffice::ValidationErrorId::EmptyAttribute));

        // ST_OnOff has no empty member, so the opposite holds for w:checkStyle: both
        // layers agree that the attribute carries no value.
        const auto emptyOnOff = validate(
            R"(<w:activeWritingStyle xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main")"
            R"( w:lang="en-US" w:vendorID="64" w:dllVersion="1" w:checkStyle="" w:appName="MSWord"/>)");
        CHECK(HasValidationIssue(emptyOnOff, ExyokiOffice::ValidationErrorId::EmptyAttribute));
    }

    TEST_CASE("a value outside its lexical space never blocks loading or round-tripping [unit] [metadata] [dom-validation]")
    {
        // Rejecting a spelling is a statement about what the *typed accessor*
        // returns, not about whether the document can be opened. A producer that
        // writes `True` for an xsd:boolean still has its package loaded, its
        // element typed, and its text written back byte for byte - only a caller
        // asking for the parsed value is told the attribute carries nothing it can
        // interpret.
        constexpr std::string_view kXml =
            R"(<x:fileRecoveryPr xmlns:x="http://schemas.openxmlformats.org/spreadsheetml/2006/main" autoRecover="True"/>)";
        const ExyokiOffice::OpenXmlQualifiedName kAutoRecover("", "autoRecover");

        auto package = std::make_shared<ExyokiOffice::OpenXmlPackage>();
        REQUIRE(package->LoadFromMemory(BuildSingleXmlPartPackage(kXml)));

        auto part = package->GetPartByUri("/custom.xml");
        REQUIRE(part != nullptr);
        auto root = part->GetRootElement();
        REQUIRE(root != nullptr);

        // The element is typed, so this is not the untyped fallback path.
        const auto properties =
            std::dynamic_pointer_cast<ExyokiOffice::DocumentFormat::OpenXml::Spreadsheet::FileRecoveryProperties>(root);
        REQUIRE(properties != nullptr);

        // The typed accessor reports "nothing I can read" rather than a made-up false.
        CHECK_FALSE(properties->GetAutoRecover().IsDefined());
        CHECK(properties->GetAutoRecover().ValueOr(true));

        // The raw text is untouched and still reachable.
        CHECK(root->GetAttribute(kAutoRecover) == "True");

        // And it survives a save/load cycle unchanged, because nothing assigned
        // over it - this is what keeps a foreign producer's file lossless.
        const auto saved = package->SaveToMemory();
        REQUIRE_FALSE(saved.empty());

        auto reloaded = std::make_shared<ExyokiOffice::OpenXmlPackage>();
        REQUIRE(reloaded->LoadFromMemory(saved));
        auto reloadedPart = reloaded->GetPartByUri("/custom.xml");
        REQUIRE(reloadedPart != nullptr);
        auto reloadedRoot = reloadedPart->GetRootElement();
        REQUIRE(reloadedRoot != nullptr);
        CHECK(reloadedRoot->GetAttribute(kAutoRecover) == "True");

        // Assigning a valid value replaces it with the canonical spelling, so a
        // caller that does mean to fix the attribute still gets schema-valid output.
        properties->SetAutoRecover(ExyokiOffice::BooleanValue(true));
        CHECK(root->GetAttribute(kAutoRecover) == "1");
    }

    TEST_CASE("the validator and the simple types agree on which empty values are valid [unit] [metadata] [dom-validation]")
    {
        // MetadataAttributeInfo::AllowsEmptyValue drives the validator, and each
        // simple type's TryParse drives reading. If the two ever disagree, a
        // document either validates but reads as unset, or reads fine and is
        // reported broken. This pins them together for every declared type name
        // that appears in the generated metadata.
        const auto agreesWith = [](std::string_view typeName, bool expected)
        {
            CAPTURE(typeName);
            ExyokiOffice::MetadataAttributeInfo attribute;
            attribute.TypeName = std::string(typeName);
            CHECK(attribute.AllowsEmptyValue() == expected);
        };

        // Types whose lexical space contains the empty string.
        agreesWith("StringValue", true);
        agreesWith("HexBinaryValue", true);
        agreesWith("Base64BinaryValue", true);
        agreesWith("TrueFalseBlankValue", true);
        agreesWith("ListValue<StringValue>", true);
        agreesWith("ListValue<UInt32Value>", true);

        // Types where an empty attribute really has no value. OnOffValue belongs
        // here: ST_OnOff is the union of xsd:boolean with on/off, and neither
        // branch has a blank member.
        agreesWith("OnOffValue", false);
        agreesWith("BooleanValue", false);
        agreesWith("TrueFalseValue", false);
        agreesWith("DateTimeValue", false);
        agreesWith("DecimalValue", false);
        agreesWith("DoubleValue", false);
        agreesWith("SingleValue", false);
        agreesWith("IntegerValue", false);
        agreesWith("ByteValue", false);
        agreesWith("SByteValue", false);
        agreesWith("Int16Value", false);
        agreesWith("Int32Value", false);
        agreesWith("Int64Value", false);
        agreesWith("UInt16Value", false);
        agreesWith("UInt32Value", false);
        agreesWith("UInt64Value", false);
        agreesWith("EnumValue<SomeValues>", false);

        // A constraint used outside generated metadata cannot know the type, so it
        // stays conservative.
        agreesWith("", false);

        // The claim above about the simple types themselves, so the two halves of
        // this test cannot drift apart silently.
        CHECK(ExyokiOffice::TrueFalseBlankValue{std::string_view("")}.IsDefined());
        CHECK_FALSE(ExyokiOffice::OnOffValue{std::string_view("")}.IsDefined());
        CHECK_FALSE(ExyokiOffice::BooleanValue{std::string_view("")}.IsDefined());
        CHECK_FALSE(ExyokiOffice::TrueFalseValue{std::string_view("")}.IsDefined());
        CHECK(ExyokiOffice::StringValue{std::string_view("")}.IsDefined());
        CHECK(ExyokiOffice::HexBinaryValue{std::string_view("")}.IsDefined());
        CHECK(ExyokiOffice::Base64BinaryValue{std::string_view("")}.IsDefined());
        CHECK_FALSE(ExyokiOffice::Int32Value{std::string_view("")}.IsDefined());
        CHECK_FALSE(ExyokiOffice::DoubleValue{std::string_view("")}.IsDefined());
        CHECK_FALSE(ExyokiOffice::DateTimeValue{std::string_view("")}.IsDefined());
    }

    TEST_CASE("an empty value is reported once, whether or not the attribute is required [unit] [metadata] [dom-validation]")
    {
        const ExyokiOffice::OpenXmlDomValidator validator;
        const auto issuesFor = [&](std::string_view xml, std::string_view attributeName)
        {
            // The result has to be named: iterating the vector a temporary
            // ValidationResult owns walks memory that is already gone.
            const auto result = validator.ValidateElement(*LoadRootElement(xml));
            std::vector<ExyokiOffice::ValidationIssue> issues;
            for (const auto& issue : result.Issues())
            {
                if (issue.Location.AttributeName == attributeName)
                {
                    issues.push_back(issue);
                }
            }
            return issues;
        };

        // a:gridCol/@w is a required Int64Value carrying a range number validator. An
        // empty value is one defect, so it gets one diagnostic: without this the
        // number validator adds an AttributeNumberParsingFailed about the same text.
        const auto required = issuesFor(
            R"(<a:gridCol xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" w=""/>)", "w");
        REQUIRE(required.size() == 1);
        CHECK(required.front().Id == ExyokiOffice::ValidationErrorId::EmptyAttribute);

        // w:activeWritingStyle/@w:dllVersion is optional, so no required validator is
        // generated for it - the emptiness still has to be caught.
        const auto optional = issuesFor(
            R"(<w:activeWritingStyle xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main")"
            R"( w:lang="en-US" w:vendorID="64" w:dllVersion="" w:checkStyle="1" w:appName="MSWord"/>)",
            "w:dllVersion");
        REQUIRE(optional.size() == 1);
        CHECK(optional.front().Id == ExyokiOffice::ValidationErrorId::EmptyAttribute);
    }

    TEST_CASE("anyURI attributes accept relative references [unit] [metadata] [dom-validation]")
    {
        const ExyokiOffice::OpenXmlDomValidator validator;
        const auto validate = [&](std::string_view xml)
        {
            return validator.ValidateElement(*LoadRootElement(xml));
        };

        // Every InkML reference attribute is xsd:anyURI, and InkML writes them as
        // same-document fragments. Demanding a scheme rejected every inked document.
        const auto fragments = validate(
            R"(<inkml:context xmlns:inkml="http://www.w3.org/2003/InkML" contextRef="#ctx0")"
            R"( canvasRef="#canvas0" traceFormatRef="#tf0"/>)");
        CHECK_FALSE(HasValidationIssue(fragments, ExyokiOffice::ValidationErrorId::AttributeUriViolation));

        const auto spaces = validate(
            R"(<inkml:context xmlns:inkml="http://www.w3.org/2003/InkML" contextRef="#ctx 0"/>)");
        CHECK(HasValidationIssue(spaces, ExyokiOffice::ValidationErrorId::AttributeUriViolation));
    }

    TEST_CASE("hexBinary length facets count octets, not characters [unit] [metadata] [dom-validation]")
    {
        const ExyokiOffice::OpenXmlDomValidator validator;
        const auto validateColor = [&](std::string_view value)
        {
            std::string xml = R"(<w:color xmlns:w=")";
            xml += kWordNs;
            xml += R"(" w:val=")";
            xml += value;
            xml += R"("/>)";
            return validator.ValidateElement(*LoadRootElement(xml));
        };

        // ST_HexColorRGB is xsd:hexBinary with length 3, meaning three octets and
        // therefore six hex digits. This is what Word itself writes.
        CHECK(validateColor("C00000").IsValid());
        CHECK(validateColor("2F5496").IsValid());

        // The other branch of the union stays intact.
        CHECK(validateColor("auto").IsValid());

        // Two octets are still the wrong length, and a non-hex value is not a
        // hexBinary literal at all.
        CHECK(HasValidationIssue(validateColor("C000"),
                                 ExyokiOffice::ValidationErrorId::AttributeExactLengthMismatch));
        CHECK(HasValidationIssue(validateColor("ZZZZZZ"),
                                 ExyokiOffice::ValidationErrorId::AttributePatternMismatch));
    }

    TEST_CASE("character length facets keep counting characters [unit] [metadata] [dom-validation]")
    {
        const ExyokiOffice::OpenXmlDomValidator validator;
        const auto validateCnfStyle = [&](std::string_view value)
        {
            std::string xml = R"(<w:cnfStyle xmlns:w=")";
            xml += kWordNs;
            xml += R"(" w:val=")";
            xml += value;
            xml += R"("/>)";
            return validator.ValidateElement(*LoadRootElement(xml));
        };

        // ST_Cnf is a bit string of length 12, where the facet counts characters.
        // The hexBinary rule must not leak into types like this one.
        CHECK(validateCnfStyle("100000000000").IsValid());
        CHECK(HasValidationIssue(validateCnfStyle("100000"),
                                 ExyokiOffice::ValidationErrorId::AttributeExactLengthMismatch));
    }

    TEST_CASE("XML locations carry positional paths and canonical prefixes [unit] [metadata] [dom-validation]")
    {
        auto root = LoadRootElement(
            R"(<t:root xmlns:t="urn:exyokioffice:test">)"
            R"(<t:item/><t:other/><t:item><t:leaf/></t:item><t:item/></t:root>)");
        REQUIRE(root != nullptr);

        const auto children = root->Children();
        REQUIRE(children.size() == 4);

        CHECK(root->GetXmlLocation().Path == "/t:root");
        CHECK(root->GetXmlLocation().ElementName == "t:root");

        // Repeated names get a 1-based predicate, a unique name gets none.
        CHECK(children[0]->GetXmlLocation().Path == "/t:root/t:item[1]");
        CHECK(children[1]->GetXmlLocation().Path == "/t:root/t:other");
        CHECK(children[2]->GetXmlLocation().Path == "/t:root/t:item[2]");
        CHECK(children[3]->GetXmlLocation().Path == "/t:root/t:item[3]");
        CHECK(children[2]->Children().front()->GetXmlLocation().Path == "/t:root/t:item[2]/t:leaf");

        // The prefix a document happens to declare must not leak into the path:
        // a known Open XML namespace is always reported with its canonical prefix.
        auto word = LoadRootElement(
            R"(<zz:body xmlns:zz="http://schemas.openxmlformats.org/wordprocessingml/2006/main">)"
            R"(<zz:p/><zz:p/></zz:body>)");
        REQUIRE(word != nullptr);
        CHECK(word->GetXmlLocation().Path == "/w:body");
        CHECK(word->Children().back()->GetXmlLocation().Path == "/w:body/w:p[2]");
        CHECK(word->Children().back()->GetXmlLocation().ElementName == "w:p");
    }

    TEST_CASE("XML locations can point at a present or a missing attribute [unit] [metadata] [dom-validation]")
    {
        auto root = LoadRootElement(
            R"(<zz:p xmlns:zz="http://schemas.openxmlformats.org/wordprocessingml/2006/main" zz:rsidR="00A1"/>)");
        REQUIRE(root != nullptr);

        const ExyokiOffice::OpenXmlQualifiedName present(kWordNs, "rsidR");
        const ExyokiOffice::OpenXmlQualifiedName absent(kWordNs, "rsidRDefault");

        // A present attribute is reported the way the document spells it, so the
        // location can be matched against the markup.
        CHECK(root->GetXmlLocation(present).AttributeName == "zz:rsidR");

        // A missing one has no spelling in the document; the prefix bound to its
        // namespace is used instead.
        CHECK(root->GetXmlLocation(absent).AttributeName == "zz:rsidRDefault");

        // The element part of the location is unaffected either way.
        CHECK(root->GetXmlLocation(absent).Path == "/w:p");
        CHECK(root->GetXmlLocation(absent).ElementName == "w:p");
    }

    TEST_CASE("package validator forwards Office target version to every DOM part [unit] [metadata] [dom-validation]")
    {
        const auto bytes = BuildSingleXmlPartPackage(
            R"(<x:workbook xmlns:x="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><x:workbookPr dateCompatibility="1"/></x:workbook>)");
        ExyokiOffice::OpenXmlPackage package;
        REQUIRE(package.LoadFromMemory(bytes));

        const auto latest = ExyokiOffice::OpenXmlPackageValidator(
                                ExyokiOffice::OpenXmlDomValidationSettings{})
                                .Validate(package);
        CHECK_FALSE(HasValidationIssue(latest,
                                       ExyokiOffice::ValidationErrorId::AttributeVersionViolation));

        const ExyokiOffice::OpenXmlPackageValidator office2007(
            {ExyokiOffice::OpenXml::FileFormatVersions::Office2007});
        const auto legacy = office2007.Validate(package);
        CHECK(HasValidationIssue(legacy,
                                 ExyokiOffice::ValidationErrorId::AttributeVersionViolation));
    }

    TEST_CASE("generated Office 2010 elements are rejected by an Office 2007 target in all document families [unit] [metadata] [dom-validation] [version]")
    {
        const ExyokiOffice::OpenXmlDomValidator office2007(
            {ExyokiOffice::OpenXml::FileFormatVersions::Office2007});
        const ExyokiOffice::OpenXmlDomValidator office2010(
            {ExyokiOffice::OpenXml::FileFormatVersions::Office2010});

        const std::array<std::string_view, 3> elements = {
            R"(<w14:extLst xmlns:w14="http://schemas.microsoft.com/office/word/2010/wordml"/>)",
            R"(<x14:extLst xmlns:x14="http://schemas.microsoft.com/office/spreadsheetml/2009/9/main"/>)",
            R"(<p14:nullEvt xmlns:p14="http://schemas.microsoft.com/office/powerpoint/2010/main"/>)"};
        for (const auto xml : elements)
        {
            CAPTURE(xml);
            const auto element = LoadRootElement(xml);
            REQUIRE(element);
            CHECK(HasValidationIssue(office2007.ValidateElement(*element),
                                     ExyokiOffice::ValidationErrorId::ElementVersionViolation));
            CHECK_FALSE(HasValidationIssue(office2010.ValidateElement(*element),
                                           ExyokiOffice::ValidationErrorId::ElementVersionViolation));
        }
    }

    TEST_CASE("schema-aware insertion orders worksheet children and raw insertion remains explicit [unit] [metadata] [dom-insertion]")
    {
        namespace S = ExyokiOffice::DocumentFormat::OpenXml::Spreadsheet;

        auto schemaRoot = LoadRootElement(
            R"(<x:worksheet xmlns:x="http://schemas.openxmlformats.org/spreadsheetml/2006/main"/>)");
        auto worksheet = ExyokiOffice::openxmlelement_cast<S::Worksheet>(schemaRoot);
        REQUIRE(worksheet);
        REQUIRE(worksheet->AppendChild<S::SheetData>());
        REQUIRE(worksheet->AppendChild<S::SheetProperties>());
        CHECK(worksheet->AppendChild<S::SheetProperties>() == nullptr);
        REQUIRE(worksheet->Children().size() == 2);
        CHECK(worksheet->Children()[0]->QualifiedName().localName() == "sheetPr");
        CHECK(worksheet->Children()[1]->QualifiedName().localName() == "sheetData");
        CHECK(ExyokiOffice::OpenXmlDomValidator().ValidateElement(*worksheet).IsValid());

        auto rawRoot = LoadRootElement(
            R"(<x:worksheet xmlns:x="http://schemas.openxmlformats.org/spreadsheetml/2006/main"/>)");
        auto rawWorksheet = ExyokiOffice::openxmlelement_cast<S::Worksheet>(rawRoot);
        REQUIRE(rawWorksheet);
        REQUIRE(rawWorksheet->AppendChildRaw<S::SheetData>());
        REQUIRE(rawWorksheet->AppendChildRaw<S::SheetProperties>());
        CHECK_FALSE(ExyokiOffice::OpenXmlDomValidator().ValidateElement(*rawWorksheet).IsValid());
    }

    TEST_CASE("schema-aware insertion handles beginning middle end repeats and explicit anchors [unit] [metadata] [dom-insertion]")
    {
        namespace W = ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing;
        namespace S = ExyokiOffice::DocumentFormat::OpenXml::Spreadsheet;
        namespace P = ExyokiOffice::DocumentFormat::OpenXml::Presentation;

        auto bodyRoot = LoadRootElement(
            R"(<w:body xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:sectPr/></w:body>)");
        auto body = ExyokiOffice::openxmlelement_cast<W::Body>(bodyRoot);
        REQUIRE(body);
        auto firstParagraph = body->AppendChild<W::Paragraph>();
        auto secondParagraph = body->AppendChild<W::Paragraph>();
        REQUIRE(firstParagraph);
        REQUIRE(secondParagraph);
        auto insertedParagraph = body->InsertChild<W::Paragraph>(firstParagraph);
        REQUIRE(insertedParagraph);
        const auto bodyChildren = body->Children();
        REQUIRE(bodyChildren.size() == 4);
        CHECK(bodyChildren[0]->IsSameNode(insertedParagraph));
        CHECK(bodyChildren[1]->IsSameNode(firstParagraph));
        CHECK(bodyChildren[2]->IsSameNode(secondParagraph));
        CHECK(bodyChildren[3]->QualifiedName().localName() == "sectPr");
        CHECK(ExyokiOffice::OpenXmlDomValidator().ValidateElement(*body).IsValid());

        auto workbookRoot = LoadRootElement(
            R"(<x:workbook xmlns:x="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><x:sheets/></x:workbook>)");
        auto workbook = ExyokiOffice::openxmlelement_cast<S::Workbook>(workbookRoot);
        REQUIRE(workbook);
        REQUIRE(workbook->AppendChild<S::CalculationProperties>());
        REQUIRE(workbook->AppendChild<S::WorkbookProperties>());
        REQUIRE(workbook->AppendChild<S::FileVersion>());
        const auto workbookChildren = workbook->Children();
        REQUIRE(workbookChildren.size() == 4);
        CHECK(workbookChildren[0]->QualifiedName().localName() == "fileVersion");
        CHECK(workbookChildren[1]->QualifiedName().localName() == "workbookPr");
        CHECK(workbookChildren[2]->QualifiedName().localName() == "sheets");
        CHECK(workbookChildren[3]->QualifiedName().localName() == "calcPr");
        CHECK(ExyokiOffice::OpenXmlDomValidator().ValidateElement(*workbook).IsValid());

        auto presentationRoot = LoadRootElement(
            R"(<p:presentation xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main"><p:notesSz/></p:presentation>)");
        auto presentation = ExyokiOffice::openxmlelement_cast<P::Presentation>(presentationRoot);
        REQUIRE(presentation);
        REQUIRE(presentation->AppendChild<P::SlideIdList>());
        REQUIRE(presentation->AppendChild<P::SlideSize>());
        const auto presentationChildren = presentation->Children();
        REQUIRE(presentationChildren.size() == 3);
        CHECK(presentationChildren[0]->QualifiedName().localName() == "sldIdLst");
        CHECK(presentationChildren[1]->QualifiedName().localName() == "sldSz");
        CHECK(presentationChildren[2]->QualifiedName().localName() == "notesSz");
        CHECK(ExyokiOffice::OpenXmlDomValidator().ValidateElement(*presentation).IsValid());
    }

    TEST_CASE("raw insertion honors exact anchor and validator diagnoses every invalid family [unit] [metadata] [dom-insertion]")
    {
        namespace W = ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing;
        namespace S = ExyokiOffice::DocumentFormat::OpenXml::Spreadsheet;
        namespace P = ExyokiOffice::DocumentFormat::OpenXml::Presentation;
        const ExyokiOffice::OpenXmlDomValidator validator;

        auto bodyRoot = LoadRootElement(
            R"(<w:body xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:sectPr/></w:body>)");
        auto body = ExyokiOffice::openxmlelement_cast<W::Body>(bodyRoot);
        REQUIRE(body);
        auto section = body->Children().front();
        auto rawBefore = body->InsertChildRaw<W::Paragraph>(section);
        REQUIRE(rawBefore);
        CHECK(body->Children()[0]->IsSameNode(rawBefore));
        auto rawAfter = body->AppendChildRaw<W::Paragraph>();
        REQUIRE(rawAfter);
        CHECK(body->Children().back()->IsSameNode(rawAfter));
        CHECK_FALSE(validator.ValidateElement(*body).IsValid());

        auto workbookRoot = LoadRootElement(
            R"(<x:workbook xmlns:x="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><x:sheets/></x:workbook>)");
        auto workbook = ExyokiOffice::openxmlelement_cast<S::Workbook>(workbookRoot);
        REQUIRE(workbook);
        REQUIRE(workbook->AppendChildRaw<S::WorkbookProperties>());
        CHECK_FALSE(validator.ValidateElement(*workbook).IsValid());

        auto presentationRoot = LoadRootElement(
            R"(<p:presentation xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main"><p:notesSz/></p:presentation>)");
        auto presentation = ExyokiOffice::openxmlelement_cast<P::Presentation>(presentationRoot);
        REQUIRE(presentation);
        REQUIRE(presentation->AppendChildRaw<P::SlideSize>());
        CHECK_FALSE(validator.ValidateElement(*presentation).IsValid());
    }

    TEST_CASE("schema-aware insertion preserves unknown children, enforces max cardinality, and raw insertion permits diagnostics [unit] [metadata] [dom-insertion]")
    {
        namespace W = ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing;
        namespace S = ExyokiOffice::DocumentFormat::OpenXml::Spreadsheet;

        auto bodyRoot = LoadRootElement(
            R"(<w:body xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"/>)");
        auto body = ExyokiOffice::openxmlelement_cast<W::Body>(bodyRoot);
        REQUIRE(body);
        REQUIRE(body->AppendChild<S::SheetData>());
        CHECK(HasValidationIssue(ExyokiOffice::OpenXmlDomValidator().ValidateElement(*body),
                                 ExyokiOffice::ValidationErrorId::ParticleConstraintViolation));

        auto worksheetRoot = LoadRootElement(
            R"(<x:worksheet xmlns:x="http://schemas.openxmlformats.org/spreadsheetml/2006/main"/>)");
        auto worksheet = ExyokiOffice::openxmlelement_cast<S::Worksheet>(worksheetRoot);
        REQUIRE(worksheet);
        REQUIRE(worksheet->AppendChild<S::SheetProperties>());
        CHECK(worksheet->AppendChild<S::SheetProperties>() == nullptr);
        REQUIRE(worksheet->AppendChildRaw<S::SheetProperties>());
        CHECK(HasValidationIssue(ExyokiOffice::OpenXmlDomValidator().ValidateElement(*worksheet),
                                 ExyokiOffice::ValidationErrorId::ParticleConstraintViolation));
    }

    TEST_CASE("required attribute constraint reports missing and empty values [unit] [metadata] [metadata-constraints]")
    {
        const ExyokiOffice::MetadataRequiredConstraint constraint(Attr("id"), "Id", true);
        const ExyokiOffice::MetadataRequiredConstraint optional(Attr("id"), "Id", false);

        auto missing = MakeAttributeElement("");
        auto empty = MakeAttributeElement(R"(t:id="")");
        auto present = MakeAttributeElement(R"(t:id="abc")");

        CHECK(IsValid(optional, *missing));
        CHECK(IsValid(optional, *empty));
        CHECK(SingleIssue(constraint, *missing).Id == ExyokiOffice::ValidationErrorId::MissingAttribute);
        CHECK(SingleIssue(constraint, *empty).Id == ExyokiOffice::ValidationErrorId::EmptyAttribute);
        CHECK(IsValid(constraint, *present));
    }

    TEST_CASE("string attribute constraint validates length, pattern, token, names and uri [unit] [metadata] [metadata-constraints]")
    {
        ExyokiOffice::MetadataStringConstraint length(Attr("value"), "Value", 2, 4, std::nullopt, std::nullopt,
                                                      false, false, false, false, false);
        CHECK(IsValid(length, *MakeAttributeElement("")));
        CHECK(IsValid(length, *MakeAttributeElement(R"(t:value="abcd")")));
        CHECK(SingleIssue(length, *MakeAttributeElement(R"(t:value="a")")).Id == ExyokiOffice::ValidationErrorId::AttributeMinLengthMismatch);
        CHECK(SingleIssue(length, *MakeAttributeElement(R"(t:value="abcde")")).Id == ExyokiOffice::ValidationErrorId::AttributeMaxLengthMismatch);

        ExyokiOffice::MetadataStringConstraint exactPattern(Attr("value"), "Value", std::nullopt, std::nullopt, 3,
                                                            "^[A-Z]{3}$", false, false, false, false, false);
        CHECK(IsValid(exactPattern, *MakeAttributeElement(R"(t:value="ABC")")));
        CHECK(HasIssue(exactPattern,
                       *MakeAttributeElement(R"(t:value="AB")"),
                       ExyokiOffice::ValidationErrorId::AttributeExactLengthMismatch));
        CHECK(SingleIssue(exactPattern, *MakeAttributeElement(R"(t:value="AbC")")).Id == ExyokiOffice::ValidationErrorId::AttributePatternMismatch);

        ExyokiOffice::MetadataStringConstraint invalidPattern(Attr("value"), "Value", std::nullopt, std::nullopt,
                                                              std::nullopt, "[", false, false, false, false, false);
        CHECK(SingleIssue(invalidPattern, *MakeAttributeElement(R"(t:value="ABC")")).Id == ExyokiOffice::ValidationErrorId::AttributePatternMismatch);

        ExyokiOffice::MetadataStringConstraint token(Attr("value"), "Value", std::nullopt, std::nullopt, std::nullopt,
                                                     std::nullopt, true, false, false, false, false);
        CHECK(IsValid(token, *MakeAttributeElement(R"(t:value="good-token")")));
        CHECK(SingleIssue(token, *MakeAttributeElement("t:value=\" leading\"")).Id == ExyokiOffice::ValidationErrorId::AttributeTokenMismatch);
        CHECK(SingleIssue(token, *MakeAttributeElement("t:value=\"trailing \"")).Id == ExyokiOffice::ValidationErrorId::AttributeTokenMismatch);
        CHECK(SingleIssue(token, *MakeAttributeElement("t:value=\"bad  token\"")).Id == ExyokiOffice::ValidationErrorId::AttributeTokenMismatch);

        ExyokiOffice::MetadataStringConstraint ncName(Attr("value"), "Value", std::nullopt, std::nullopt, std::nullopt,
                                                      std::nullopt, false, true, false, false, false);
        CHECK(IsValid(ncName, *MakeAttributeElement(R"(t:value="good_name")")));
        CHECK_FALSE(IsValid(ncName, *MakeAttributeElement(R"(t:value="bad:name")")));
        CHECK_FALSE(IsValid(ncName, *MakeAttributeElement(R"(t:value="bad name")")));
        CHECK(SingleIssue(ncName, *MakeAttributeElement(R"(t:value="1bad")")).Id == ExyokiOffice::ValidationErrorId::AttributeNcNameViolation);

        ExyokiOffice::MetadataStringConstraint qName(Attr("value"), "Value", std::nullopt, std::nullopt, std::nullopt,
                                                     std::nullopt, false, false, true, false, false);
        CHECK(IsValid(qName, *MakeAttributeElement(R"(t:value="localName")")));
        CHECK(IsValid(qName, *MakeAttributeElement(R"(t:value="p:goodName")")));
        CHECK_FALSE(IsValid(qName, *MakeAttributeElement(R"(t:value=":bad")")));
        CHECK_FALSE(IsValid(qName, *MakeAttributeElement(R"(t:value="bad:")")));
        CHECK(SingleIssue(qName, *MakeAttributeElement(R"(t:value="bad:name:again")")).Id == ExyokiOffice::ValidationErrorId::AttributeQNameViolation);

        ExyokiOffice::MetadataStringConstraint id(Attr("value"), "Value", std::nullopt, std::nullopt, std::nullopt,
                                                  std::nullopt, false, false, false, true, false);
        CHECK(IsValid(id, *MakeAttributeElement(R"(t:value="goodId")")));
        CHECK(SingleIssue(id, *MakeAttributeElement(R"(t:value="1bad")")).Id == ExyokiOffice::ValidationErrorId::AttributeIdViolation);

        // xsd:anyURI is a URI *reference*: absolute forms, relative paths, bare
        // fragments and the empty string are all values of the type. Only text that
        // cannot be a reference at all is rejected.
        ExyokiOffice::MetadataStringConstraint uri(Attr("value"), "Value", std::nullopt, std::nullopt, std::nullopt,
                                                   std::nullopt, false, false, false, false, true);
        CHECK(IsValid(uri, *MakeAttributeElement(R"(t:value="http://example.test")")));
        CHECK(IsValid(uri, *MakeAttributeElement(R"(t:value="urn:example:test")")));
        CHECK(IsValid(uri, *MakeAttributeElement(R"(t:value="http://[2001:db8::1]:8080/p")")));
        CHECK(IsValid(uri, *MakeAttributeElement(R"(t:value="#ctx0")")));
        CHECK(IsValid(uri, *MakeAttributeElement(R"(t:value="media/image1.png")")));
        CHECK(IsValid(uri, *MakeAttributeElement(R"(t:value="../word/document.xml")")));
        CHECK(IsValid(uri, *MakeAttributeElement(R"(t:value="a%20b")")));
        CHECK(IsValid(uri, *MakeAttributeElement(R"(t:value="")")));
        CHECK(SingleIssue(uri, *MakeAttributeElement("t:value=\"http://bad uri\"")).Id == ExyokiOffice::ValidationErrorId::AttributeUriViolation);
        CHECK(SingleIssue(uri, *MakeAttributeElement(R"(t:value="a&#10;b")")).Id == ExyokiOffice::ValidationErrorId::AttributeUriViolation);
        CHECK(SingleIssue(uri, *MakeAttributeElement(R"(t:value="a%2gb")")).Id == ExyokiOffice::ValidationErrorId::AttributeUriViolation);
        CHECK(SingleIssue(uri, *MakeAttributeElement(R"(t:value="a%2")")).Id == ExyokiOffice::ValidationErrorId::AttributeUriViolation);
        CHECK(SingleIssue(uri, *MakeAttributeElement(R"(t:value="path/[brackets]")")).Id == ExyokiOffice::ValidationErrorId::AttributeUriViolation);
    }

    TEST_CASE("number attribute constraint validates parsing, ranges and sign rules [unit] [metadata] [metadata-constraints]")
    {
        ExyokiOffice::MetadataNumberConstraint bounded(Attr("value"), "Value", "Int32Value", 1.0, 5.0, std::nullopt,
                                                       std::nullopt, false, false, false);
        CHECK(IsValid(bounded, *MakeAttributeElement("")));
        CHECK(IsValid(bounded, *MakeAttributeElement(R"(t:value="3")")));
        CHECK(SingleIssue(bounded, *MakeAttributeElement(R"(t:value="abc")")).Id == ExyokiOffice::ValidationErrorId::AttributeNumberParsingFailed);
        CHECK(SingleIssue(bounded, *MakeAttributeElement(R"(t:value="0")")).Id == ExyokiOffice::ValidationErrorId::AttributeMinInclusiveViolation);
        CHECK(SingleIssue(bounded, *MakeAttributeElement(R"(t:value="6")")).Id == ExyokiOffice::ValidationErrorId::AttributeMaxInclusiveViolation);

        ExyokiOffice::MetadataNumberConstraint exclusive(Attr("value"), "Value", "DoubleValue", std::nullopt,
                                                         std::nullopt, 1.0, 5.0, false, false, false);
        CHECK(IsValid(exclusive, *MakeAttributeElement(R"(t:value="3.5")")));
        CHECK(SingleIssue(exclusive, *MakeAttributeElement(R"(t:value="1")")).Id == ExyokiOffice::ValidationErrorId::AttributeMinExclusiveViolation);
        CHECK(SingleIssue(exclusive, *MakeAttributeElement(R"(t:value="5")")).Id == ExyokiOffice::ValidationErrorId::AttributeMaxExclusiveViolation);

        ExyokiOffice::MetadataNumberConstraint sign(Attr("value"), "Value", "DoubleValue", std::nullopt,
                                                    std::nullopt, std::nullopt, std::nullopt, true, true, false);
        CHECK(IsValid(sign, *MakeAttributeElement(R"(t:value="1")")));
        CHECK(SingleIssue(sign, *MakeAttributeElement(R"(t:value="0")")).Id == ExyokiOffice::ValidationErrorId::AttributePositiveViolation);
        CHECK(HasIssue(sign,
                       *MakeAttributeElement(R"(t:value="-1")"),
                       ExyokiOffice::ValidationErrorId::AttributePositiveViolation));
        CHECK(HasIssue(sign,
                       *MakeAttributeElement(R"(t:value="-1")"),
                       ExyokiOffice::ValidationErrorId::AttributeNonNegativeViolation));

        ExyokiOffice::MetadataNumberConstraint list(Attr("value"), "Value", "Int32Value", 0.0, 10.0, std::nullopt,
                                                    std::nullopt, false, false, true);
        CHECK(IsValid(list, *MakeAttributeElement(R"(t:value="1 2 10")")));
        CHECK(SingleIssue(list, *MakeAttributeElement(R"(t:value="1 11")")).Id == ExyokiOffice::ValidationErrorId::AttributeMaxInclusiveViolation);
        CHECK(SingleIssue(list, *MakeAttributeElement(R"(t:value="   ")")).Id == ExyokiOffice::ValidationErrorId::AttributeNumberParsingFailed);
    }

    TEST_CASE("enum attribute constraints validate allowed values and union rules [unit] [metadata] [metadata-constraints]")
    {
        ExyokiOffice::MetadataEnumConstraint enumConstraint(Attr("value"), "Value", [](std::string_view value)
                                                            { return value == "one" || value == "two"; });
        CHECK(IsValid(enumConstraint, *MakeAttributeElement("")));
        CHECK(IsValid(enumConstraint, *MakeAttributeElement(R"(t:value="")")));
        CHECK(IsValid(enumConstraint, *MakeAttributeElement(R"(t:value="one")")));
        CHECK(SingleIssue(enumConstraint, *MakeAttributeElement(R"(t:value="three")")).Id == ExyokiOffice::ValidationErrorId::AttributeEnumViolation);

        ExyokiOffice::MetadataEnumConstraint noValidator(Attr("value"), "Value", {});
        CHECK(IsValid(noValidator, *MakeAttributeElement(R"(t:value="three")")));

        ExyokiOffice::MetadataAttributeEnumUnionConstraint unionConstraint(
            Attr("value"),
            "Value",
            {ExyokiOffice::MetadataEnumRule{{"alpha", "beta"}}});
        CHECK(unionConstraint.Rules().size() == 1);
        CHECK(IsValid(unionConstraint, *MakeAttributeElement("")));
        CHECK(IsValid(unionConstraint, *MakeAttributeElement(R"(t:value="")")));
        CHECK(IsValid(unionConstraint, *MakeAttributeElement(R"(t:value="alpha")")));
        CHECK(SingleIssue(unionConstraint, *MakeAttributeElement(R"(t:value="gamma")")).Id == ExyokiOffice::ValidationErrorId::AttributeEnumViolation);
    }

    TEST_CASE("union constraint succeeds when any alternative succeeds and merges failures otherwise [unit] [metadata] [metadata-constraints]")
    {
        auto unionConstraint =
            std::make_shared<ExyokiOffice::MetadataUnionConstraint>(ExyokiOffice::MetadataConstraintType::Custom, 7);
        unionConstraint->AddAlternative(std::make_shared<ExyokiOffice::MetadataRequiredConstraint>(Attr("a"), "A", true));
        unionConstraint->AddAlternative(std::make_shared<ExyokiOffice::MetadataRequiredConstraint>(Attr("b"), "B", true));

        CHECK(IsValid(*unionConstraint, *MakeAttributeElement(R"(t:b="yes")")));

        ExyokiOffice::XmlLocationCache locations;
        const auto result = unionConstraint->Validate(*MakeAttributeElement(""), locations);
        CHECK_FALSE(result.IsValid());
        CHECK(result.Issues().size() == 2);

        auto emptyUnion =
            std::make_shared<ExyokiOffice::MetadataUnionConstraint>(ExyokiOffice::MetadataConstraintType::Custom, 8);
        emptyUnion->AddAlternative(nullptr);
        CHECK(emptyUnion->UnionId() == 8);
        CHECK(emptyUnion->Alternatives().empty());
        CHECK(SingleIssue(*emptyUnion, *MakeAttributeElement("")).Id == ExyokiOffice::ValidationErrorId::Unknown);
    }

    TEST_CASE("text constraints validate leaf text values [unit] [metadata] [metadata-constraints]")
    {
        ExyokiOffice::MetadataTextStringConstraint stringConstraint(2, 4, std::nullopt, "^[a-z]+$", true, true, true,
                                                                    true, false);
        CHECK(IsValid(stringConstraint, *MakeWordTextElement("abc")));
        CHECK(SingleIssue(stringConstraint, *MakeWordTextElement("a")).Id == ExyokiOffice::ValidationErrorId::TextMinLengthMismatch);
        CHECK(SingleIssue(stringConstraint, *MakeWordTextElement("abcde")).Id == ExyokiOffice::ValidationErrorId::TextMaxLengthMismatch);
        CHECK(SingleIssue(stringConstraint, *MakeWordTextElement("ABC")).Id == ExyokiOffice::ValidationErrorId::TextPatternMismatch);
        CHECK(HasIssue(stringConstraint,
                       *MakeWordTextElement("bad  token"),
                       ExyokiOffice::ValidationErrorId::TextTokenMismatch));
        CHECK(HasIssue(stringConstraint,
                       *MakeWordTextElement("1bad"),
                       ExyokiOffice::ValidationErrorId::TextNcNameViolation));
        CHECK(HasIssue(stringConstraint,
                       *MakeWordTextElement("bad:name:again"),
                       ExyokiOffice::ValidationErrorId::TextQNameViolation));
        CHECK(HasIssue(stringConstraint,
                       *MakeWordTextElement("1bad"),
                       ExyokiOffice::ValidationErrorId::TextIdViolation));

        ExyokiOffice::MetadataTextStringConstraint exactUri(std::nullopt, std::nullopt, 16, std::nullopt, false, false,
                                                            false, false, true);
        CHECK(IsValid(exactUri, *MakeWordTextElement("urn:example:test")));
        CHECK(SingleIssue(exactUri, *MakeWordTextElement("urn:test")).Id == ExyokiOffice::ValidationErrorId::TextExactLengthMismatch);
        CHECK(IsValid(exactUri, *MakeWordTextElement("relative/ref.png")));
        CHECK(SingleIssue(exactUri, *MakeWordTextElement("not a uri at all")).Id == ExyokiOffice::ValidationErrorId::TextUriViolation);

        ExyokiOffice::MetadataTextNumberConstraint numberConstraint("Int32Value", 1.0, 5.0, std::nullopt, std::nullopt,
                                                                    false, false, true);
        CHECK(IsValid(numberConstraint, *MakeWordTextElement("1 3 5")));
        CHECK(SingleIssue(numberConstraint, *MakeWordTextElement("bad")).Id == ExyokiOffice::ValidationErrorId::TextNumberParsingFailed);
        CHECK(SingleIssue(numberConstraint, *MakeWordTextElement("0")).Id == ExyokiOffice::ValidationErrorId::TextMinInclusiveViolation);
        CHECK(SingleIssue(numberConstraint, *MakeWordTextElement("6")).Id == ExyokiOffice::ValidationErrorId::TextMaxInclusiveViolation);

        ExyokiOffice::MetadataTextNumberConstraint exclusiveSign("DoubleValue", std::nullopt, std::nullopt, 1.0, 5.0,
                                                                 true, true, false);
        CHECK(IsValid(exclusiveSign, *MakeWordTextElement("3")));
        CHECK(SingleIssue(exclusiveSign, *MakeWordTextElement("1")).Id == ExyokiOffice::ValidationErrorId::TextMinExclusiveViolation);
        CHECK(SingleIssue(exclusiveSign, *MakeWordTextElement("5")).Id == ExyokiOffice::ValidationErrorId::TextMaxExclusiveViolation);
        CHECK(HasIssue(exclusiveSign, *MakeWordTextElement("0"), ExyokiOffice::ValidationErrorId::TextPositiveViolation));
        CHECK(HasIssue(exclusiveSign,
                       *MakeWordTextElement("-1"),
                       ExyokiOffice::ValidationErrorId::TextNonNegativeViolation));

        ExyokiOffice::MetadataTextEnumConstraint enumConstraint({ExyokiOffice::MetadataEnumRule{{"one", "two"}}});
        CHECK(IsValid(enumConstraint, *MakeWordTextElement("")));
        CHECK(IsValid(enumConstraint, *MakeWordTextElement("two")));
        CHECK(SingleIssue(enumConstraint, *MakeWordTextElement("three")).Id == ExyokiOffice::ValidationErrorId::TextEnumViolation);
    }

    TEST_CASE("metadata builder and particle accessors preserve generated metadata shape [unit] [metadata] [metadata-constraints]")
    {
        ExyokiOffice::MetadataBuilder builder;
        builder.SetElementName(ExyokiOffice::OpenXmlQualifiedName(kTestNs, "root"))
            .SetTypeName(ExyokiOffice::OpenXmlQualifiedName(kTestNs, "RootType"))
            .SetSchemaName("testSchema")
            .SetAvailability(ExyokiOffice::OpenXml::FileFormatVersions::Office2010);

        auto& attribute = builder.AddAttribute(Attr("id"),
                                               "Id",
                                               "StringValue",
                                               ExyokiOffice::OpenXml::FileFormatVersions::Office2007,
                                               "Identifier");
        attribute.Validators.push_back(
            std::make_shared<ExyokiOffice::MetadataRequiredConstraint>(Attr("id"), "Id", true));
        builder.AddAdditionalElement(ExyokiOffice::OpenXmlQualifiedName(kTestNs, "child"), "ChildType");
        builder.AddConstraint(nullptr);
        builder.AddConstraint(std::make_shared<ExyokiOffice::MetadataRequiredConstraint>(Attr("id"), "Id", true));

        auto sequence = std::make_shared<ExyokiOffice::MetadataSequenceParticle>(
            1,
            std::nullopt,
            ExyokiOffice::OpenXml::FileFormatVersions::Office2007,
            true);
        sequence->AddChild(nullptr);
        sequence->AddChild(std::make_shared<ExyokiOffice::MetadataElementParticle>(
            ExyokiOffice::OpenXmlQualifiedName(kTestNs, "child"),
            "ChildType",
            "Child",
            0,
            2,
            ExyokiOffice::OpenXml::FileFormatVersions::Office2010));
        builder.SetParticleTree(sequence);

        const auto metadata = builder.Build();
        REQUIRE(metadata != nullptr);
        CHECK(metadata->Summary().ElementName == ExyokiOffice::OpenXmlQualifiedName(kTestNs, "root"));
        CHECK(metadata->Summary().TypeName == ExyokiOffice::OpenXmlQualifiedName(kTestNs, "RootType"));
        CHECK(metadata->Summary().SchemaName == "testSchema");
        CHECK(metadata->Summary().Availability == ExyokiOffice::OpenXml::FileFormatVersions::Office2010);
        REQUIRE(metadata->Attributes().size() == 1);
        CHECK(metadata->Attributes().front().Name == Attr("id"));
        CHECK(metadata->Attributes().front().PropertyName == "Id");
        CHECK(metadata->Attributes().front().Validators.size() == 1);
        REQUIRE(metadata->AdditionalElements().size() == 1);
        CHECK(metadata->AdditionalElements().front().TypeName == "ChildType");
        CHECK(metadata->Constraints().size() == 1);

        const auto particle = metadata->ParticleTree();
        REQUIRE(particle != nullptr);
        CHECK(particle->Kind() == ExyokiOffice::MetadataParticleKind::Sequence);
        CHECK(particle->MinOccurs() == 1);
        CHECK(particle->IsUnbounded());
        CHECK(particle->RequiresFilter());

        const auto composite = std::dynamic_pointer_cast<ExyokiOffice::MetadataCompositeParticle>(particle);
        REQUIRE(composite != nullptr);
        REQUIRE(composite->Children().size() == 1);
        const auto child = std::dynamic_pointer_cast<ExyokiOffice::MetadataElementParticle>(composite->Children().front());
        REQUIRE(child != nullptr);
        CHECK(child->Element() == ExyokiOffice::OpenXmlQualifiedName(kTestNs, "child"));
        CHECK(child->ElementType() == "ChildType");
        CHECK(child->PropertyName() == "Child");
        CHECK(child->MaxOccurs() == 2);

        const ExyokiOffice::MetadataChoiceParticle choice(
            0,
            1,
            ExyokiOffice::OpenXml::FileFormatVersions::Office2007);
        CHECK(choice.Kind() == ExyokiOffice::MetadataParticleKind::Choice);
        CHECK_FALSE(choice.IsUnbounded());

        const ExyokiOffice::MetadataAnyParticle any(
            "##other",
            0,
            std::nullopt,
            ExyokiOffice::OpenXml::FileFormatVersions::Office2007);
        CHECK(any.Kind() == ExyokiOffice::MetadataParticleKind::Any);
        CHECK(any.Wildcard() == "##other");
    }

    TEST_CASE("office version constraint validates element availability [unit] [metadata] [metadata-constraints]")
    {
        ExyokiOffice::MetadataOfficeVersionConstraint constraint(Attr("unused"), "Unused",
                                                                 ExyokiOffice::OpenXml::FileFormatVersions::Office2007);

        const auto generic = MakeAttributeElement("");
        const auto issue = SingleIssue(constraint, *generic);
        CHECK(issue.Id == ExyokiOffice::ValidationErrorId::AttributeVersionViolation);
    }

    TEST_CASE("schematron presence and regex constraints validate attributes [unit] [metadata] [metadata-constraints]")
    {
        ExyokiOffice::MetadataSchematronAttributePresenceConstraint presence(Attr("id"), "@t:id");
        CHECK(IsValid(presence, *MakeAttributeElement(R"(t:id="x")")));
        CHECK(SingleIssue(presence, *MakeAttributeElement("")).Id == ExyokiOffice::ValidationErrorId::MissingAttribute);

        ExyokiOffice::MetadataSchematronAttributeRegexConstraint regex(Attr("code"), "^[A-Z]{2}[0-9]{2}$",
                                                                       "matches(@t:code)");
        CHECK(IsValid(regex, *MakeAttributeElement("")));
        CHECK(IsValid(regex, *MakeAttributeElement(R"(t:code="AB12")")));
        CHECK_FALSE(IsValid(regex, *MakeAttributeElement(R"(t:code="ab12")")));

        ExyokiOffice::MetadataSchematronAttributeRegexConstraint invalidRegex(Attr("code"), "[", "matches(@t:code)");
        CHECK_FALSE(IsValid(invalidRegex, *MakeAttributeElement(R"(t:code="AB12")")));
    }

    TEST_CASE("schematron violations name the rule that fired [unit] [metadata] [metadata-constraints]")
    {
        // Every schematron rule shares one error id, so the kind of rule has to be
        // readable from the identifier - otherwise a caller can neither filter these
        // out nor tell an allowed-values rejection from a numeric range one.
        ExyokiOffice::MetadataSchematronAttributeAllowedValuesConstraint allowed(
            Attr("kind"), {"first", "second"}, "@t:kind = ('first', 'second')");
        const auto allowedIssue = SingleIssue(allowed, *MakeAttributeElement(R"(t:kind="third")"));
        CHECK(allowedIssue.Id == ExyokiOffice::ValidationErrorId::SchematronConstraintViolation);
        CHECK(allowed.Identifier() == "SchematronAttributeAllowedValues");

        ExyokiOffice::MetadataSchematronAttributeNumericRangeConstraint range(Attr("value"), 1.0, 5.0,
                                                                              "@t:value >= 1 and @t:value <= 5");
        const auto rangeIssue = SingleIssue(range, *MakeAttributeElement(R"(t:value="9")"));
        CHECK(rangeIssue.Id == ExyokiOffice::ValidationErrorId::SchematronConstraintViolation);
        CHECK(range.Identifier() == "SchematronAttributeNumericRange");

        // A rule whose pattern does not compile was never evaluated, which is a
        // different thing from the element failing it.
        ExyokiOffice::MetadataSchematronAttributeRegexConstraint invalidRegex(Attr("code"), "[", "matches(@t:code)");
        const auto regexIssue = SingleIssue(invalidRegex, *MakeAttributeElement(R"(t:code="AB12")"));
        CHECK(regexIssue.Id == ExyokiOffice::ValidationErrorId::SchematronRuleNotEvaluable);
        CHECK(invalidRegex.Identifier() == "SchematronAttributeRegex");
    }

    TEST_CASE("schematron string length constraints support ranges and comparisons [unit] [metadata] [metadata-constraints]")
    {
        ExyokiOffice::MetadataSchematronAttributeStringLengthConstraint range(Attr("name"), 2, 4,
                                                                              "string-length(@t:name)");
        CHECK(IsValid(range, *MakeAttributeElement("")));
        CHECK(IsValid(range, *MakeAttributeElement(R"(t:name="abcd")")));
        CHECK_FALSE(IsValid(range, *MakeAttributeElement(R"(t:name="a")")));
        CHECK_FALSE(IsValid(range, *MakeAttributeElement(R"(t:name="abcde")")));

        ExyokiOffice::MetadataSchematronAttributeStringLengthConstraint lessThan(
            Attr("name"),
            ExyokiOffice::MetadataSchematronComparisonOperator::LessThan,
            5,
            "string-length(@t:name) < 5");
        CHECK(IsValid(lessThan, *MakeAttributeElement(R"(t:name="abcd")")));
        CHECK_FALSE(IsValid(lessThan, *MakeAttributeElement(R"(t:name="abcde")")));

        ExyokiOffice::MetadataSchematronAttributeStringLengthConstraint greaterThanOrEqual(
            Attr("name"),
            ExyokiOffice::MetadataSchematronComparisonOperator::GreaterThanOrEqual,
            2,
            "string-length(@t:name) >= 2");
        CHECK(IsValid(greaterThanOrEqual, *MakeAttributeElement(R"(t:name="ab")")));
        CHECK_FALSE(IsValid(greaterThanOrEqual, *MakeAttributeElement(R"(t:name="a")")));
    }

    TEST_CASE("schematron numeric constraints support ranges and all comparison operators [unit] [metadata] [metadata-constraints]")
    {
        ExyokiOffice::MetadataSchematronAttributeNumericRangeConstraint range(Attr("value"), 1.5, 5.5,
                                                                              "@t:value >= 1.5 and @t:value <= 5.5");
        CHECK(IsValid(range, *MakeAttributeElement("")));
        CHECK(IsValid(range, *MakeAttributeElement(R"(t:value="3")")));
        CHECK_FALSE(IsValid(range, *MakeAttributeElement(R"(t:value="1.4")")));
        CHECK_FALSE(IsValid(range, *MakeAttributeElement(R"(t:value="bad")")));

        ExyokiOffice::MetadataSchematronAttributeNumericRangeConstraint mixed(
            Attr("value"),
            ExyokiOffice::MetadataSchematronComparisonOperator::GreaterThanOrEqual,
            0.0,
            ExyokiOffice::MetadataSchematronComparisonOperator::LessThan,
            49.0,
            "@t:value >= 0 and @t:value < 49");
        CHECK(IsValid(mixed, *MakeAttributeElement(R"(t:value="0")")));
        CHECK(IsValid(mixed, *MakeAttributeElement(R"(t:value="48.99")")));
        CHECK_FALSE(IsValid(mixed, *MakeAttributeElement(R"(t:value="-1")")));
        CHECK_FALSE(IsValid(mixed, *MakeAttributeElement(R"(t:value="49")")));

        struct ComparisonCase
        {
            ExyokiOffice::MetadataSchematronComparisonOperator Operator;
            const char* ValidValue;
            const char* InvalidValue;
        };

        const ComparisonCase cases[] = {
            {ExyokiOffice::MetadataSchematronComparisonOperator::LessThan, "4", "5"},
            {ExyokiOffice::MetadataSchematronComparisonOperator::LessThanOrEqual, "5", "6"},
            {ExyokiOffice::MetadataSchematronComparisonOperator::GreaterThan, "6", "5"},
            {ExyokiOffice::MetadataSchematronComparisonOperator::GreaterThanOrEqual, "5", "4"},
        };

        for (const auto& item : cases)
        {
            ExyokiOffice::MetadataSchematronAttributeNumericComparisonConstraint comparison(Attr("value"), item.Operator,
                                                                                            5.0, "@t:value cmp 5");
            std::string valid = "t:value=\"";
            valid += item.ValidValue;
            valid += "\"";
            std::string invalid = "t:value=\"";
            invalid += item.InvalidValue;
            invalid += "\"";
            CHECK(IsValid(comparison, *MakeAttributeElement("")));
            CHECK(IsValid(comparison, *MakeAttributeElement(valid)));
            CHECK_FALSE(IsValid(comparison, *MakeAttributeElement(invalid)));
            CHECK_FALSE(IsValid(comparison, *MakeAttributeElement(R"(t:value="not-number")")));
        }
    }

    TEST_CASE("schematron hexadecimal rules read their attribute in base 16 [unit] [metadata] [metadata-constraints]")
    {
        // `@w14:paraId > 0 and @w14:paraId < 0x80000000` bounds an
        // ST_LongHexNumber, written as eight hexadecimal digits with no prefix.
        // Read as decimal, every value starting with a letter parses as 0 and
        // fails the lower bound - which rejected most paragraphs Word writes.
        ExyokiOffice::MetadataSchematronAttributeNumericRangeConstraint paragraphId(
            Attr("paraId"),
            ExyokiOffice::MetadataSchematronComparisonOperator::GreaterThan,
            0.0,
            ExyokiOffice::MetadataSchematronComparisonOperator::LessThan,
            2147483648.0,
            "@t:paraId > 0 and @t:paraId < 0x80000000",
            ExyokiOffice::MetadataSchematronNumberFormat::Hexadecimal);

        CHECK(IsValid(paragraphId, *MakeAttributeElement("")));
        CHECK(IsValid(paragraphId, *MakeAttributeElement(R"(t:paraId="0A1B2C3D")")));
        CHECK(IsValid(paragraphId, *MakeAttributeElement(R"(t:paraId="7FFFFFFF")")));
        CHECK(IsValid(paragraphId, *MakeAttributeElement(R"(t:paraId="00000001")")));
        CHECK_FALSE(IsValid(paragraphId, *MakeAttributeElement(R"(t:paraId="00000000")")));
        CHECK_FALSE(IsValid(paragraphId, *MakeAttributeElement(R"(t:paraId="80000000")")));
        CHECK_FALSE(IsValid(paragraphId, *MakeAttributeElement(R"(t:paraId="FFFFFFFF")")));
        CHECK_FALSE(IsValid(paragraphId, *MakeAttributeElement(R"(t:paraId="not-hex")")));

        // A decimal rule on the same shape of value keeps reading base 10, so
        // the radix travels with the rule rather than being guessed per value.
        ExyokiOffice::MetadataSchematronAttributeNumericRangeConstraint decimalRange(
            Attr("paraId"),
            ExyokiOffice::MetadataSchematronComparisonOperator::GreaterThan,
            0.0,
            ExyokiOffice::MetadataSchematronComparisonOperator::LessThan,
            2147483648.0,
            "@t:paraId > 0 and @t:paraId < 2147483648");
        CHECK_FALSE(IsValid(decimalRange, *MakeAttributeElement(R"(t:paraId="0A1B2C3D")")));

        ExyokiOffice::MetadataSchematronAttributeNumericComparisonConstraint comparison(
            Attr("value"),
            ExyokiOffice::MetadataSchematronComparisonOperator::LessThan,
            256.0,
            "@t:value < 0x100",
            ExyokiOffice::MetadataSchematronNumberFormat::Hexadecimal);
        CHECK(IsValid(comparison, *MakeAttributeElement(R"(t:value="FF")")));
        CHECK_FALSE(IsValid(comparison, *MakeAttributeElement(R"(t:value="100")")));

        // A forbidden-value rule that spells its literals in hexadecimal is
        // stating numbers, not spellings: the attribute writes the same value
        // unprefixed and zero-padded, and a plain string compare never matches.
        ExyokiOffice::MetadataSchematronAttributeForbiddenValuesConstraint forbidden(
            Attr("value"),
            std::vector<std::string>{"0x0040", "0x0800"},
            "@t:value != 0x0040 and @t:value != 0x0800",
            ExyokiOffice::MetadataSchematronNumberFormat::Hexadecimal);
        CHECK(IsValid(forbidden, *MakeAttributeElement(R"(t:value="00000041")")));
        CHECK_FALSE(IsValid(forbidden, *MakeAttributeElement(R"(t:value="00000040")")));
        CHECK_FALSE(IsValid(forbidden, *MakeAttributeElement(R"(t:value="0800")")));
    }

    TEST_CASE("schematron equality constraints compare literal values [unit] [metadata] [metadata-constraints]")
    {
        ExyokiOffice::MetadataSchematronAttributeEqualityConstraint equality(Attr("mode"), "strict",
                                                                             "@t:mode = strict");
        CHECK(IsValid(equality, *MakeAttributeElement("")));
        CHECK(IsValid(equality, *MakeAttributeElement(R"(t:mode="strict")")));
        CHECK_FALSE(IsValid(equality, *MakeAttributeElement(R"(t:mode="loose")")));

        ExyokiOffice::MetadataSchematronAttributeInequalityConstraint inequality(Attr("mode"), "forbidden",
                                                                                 "@t:mode != forbidden");
        CHECK(IsValid(inequality, *MakeAttributeElement("")));
        CHECK(IsValid(inequality, *MakeAttributeElement(R"(t:mode="allowed")")));
        CHECK_FALSE(IsValid(inequality, *MakeAttributeElement(R"(t:mode="forbidden")")));
    }

    TEST_CASE("schematron allowed-values constraints accept only listed literals [unit] [metadata] [metadata-constraints]")
    {
        ExyokiOffice::MetadataSchematronAttributeAllowedValuesConstraint constraint(
            Attr("mode"),
            std::vector<std::string>{"auto", "manual", "none"},
            "@t:mode = auto or @t:mode = manual or @t:mode = none");

        CHECK(IsValid(constraint, *MakeAttributeElement("")));
        CHECK(IsValid(constraint, *MakeAttributeElement(R"(t:mode="auto")")));
        CHECK(IsValid(constraint, *MakeAttributeElement(R"(t:mode="manual")")));
        CHECK(IsValid(constraint, *MakeAttributeElement(R"(t:mode="none")")));

        const auto issue = SingleIssue(constraint, *MakeAttributeElement(R"(t:mode="other")"));
        CHECK(issue.Id == ExyokiOffice::ValidationErrorId::SchematronConstraintViolation);
        CHECK(issue.Message.find("Schematron rule") != std::string::npos);
    }

    TEST_CASE("schematron attribute implication constraints validate dependent values [unit] [metadata] [metadata-constraints]")
    {
        ExyokiOffice::MetadataSchematronAttributeRequiredValueConstraint requiredValue(
            Attr("label"),
            Attr("function"),
            "custom",
            "@t:label and @t:function = custom");

        CHECK(IsValid(requiredValue, *MakeAttributeElement("")));
        CHECK(IsValid(requiredValue, *MakeAttributeElement(R"(t:label="yes" t:function="custom")")));
        CHECK_FALSE(IsValid(requiredValue, *MakeAttributeElement(R"(t:label="yes")")));
        CHECK_FALSE(IsValid(requiredValue, *MakeAttributeElement(R"(t:label="yes" t:function="sum")")));

        ExyokiOffice::MetadataSchematronAttributeForbiddenValueConstraint forbiddenValue(
            Attr("aboveAverage"),
            Attr("type"),
            "aboveAverage",
            "@t:aboveAverage and @t:type != aboveAverage");

        CHECK(IsValid(forbiddenValue, *MakeAttributeElement("")));
        CHECK(IsValid(forbiddenValue, *MakeAttributeElement(R"(t:aboveAverage="1")")));
        CHECK(IsValid(forbiddenValue, *MakeAttributeElement(R"(t:aboveAverage="1" t:type="top10")")));
        CHECK_FALSE(IsValid(forbiddenValue, *MakeAttributeElement(R"(t:aboveAverage="1" t:type="aboveAverage")")));
    }

    TEST_CASE("schematron mutual-exclusion constraints reject multiple present attributes [unit] [metadata] [metadata-constraints]")
    {
        ExyokiOffice::MetadataSchematronAttributeMutualExclusionConstraint constraint(
            std::vector<ExyokiOffice::OpenXmlQualifiedName>{Attr("auto"), Attr("indexed"), Attr("rgb"), Attr("theme")},
            "(@t:auto and @t:indexed) or (@t:auto and @t:rgb)");

        CHECK(IsValid(constraint, *MakeAttributeElement("")));
        CHECK(IsValid(constraint, *MakeAttributeElement(R"(t:auto="1")")));
        CHECK(IsValid(constraint, *MakeAttributeElement(R"(t:rgb="FF0000")")));
        CHECK_FALSE(IsValid(constraint, *MakeAttributeElement(R"(t:auto="1" t:rgb="FF0000")")));
        CHECK_FALSE(IsValid(constraint, *MakeAttributeElement(R"(t:indexed="1" t:theme="2")")));
    }

    TEST_CASE("schematron additional local constraints validate generated operands directly [unit] [metadata] [metadata-constraints]")
    {
        ExyokiOffice::MetadataSchematronAttributeForbiddenValuesConstraint forbiddenValues(
            Attr("value"),
            std::vector<std::string>{"INF", "-INF", "NaN"},
            "@t:value != INF and @t:value != -INF and @t:value != NaN");
        CHECK(IsValid(forbiddenValues, *MakeAttributeElement("")));
        CHECK(IsValid(forbiddenValues, *MakeAttributeElement(R"(t:value="1")")));
        CHECK_FALSE(IsValid(forbiddenValues, *MakeAttributeElement(R"(t:value="NaN")")));

        struct AttributeComparisonCase
        {
            ExyokiOffice::MetadataSchematronComparisonOperator Operator;
            const char* ValidAttributes;
            const char* InvalidAttributes;
        };

        const AttributeComparisonCase cases[] = {
            {ExyokiOffice::MetadataSchematronComparisonOperator::LessThan,
             R"(t:min="1" t:max="2")",
             R"(t:min="2" t:max="2")"},
            {ExyokiOffice::MetadataSchematronComparisonOperator::LessThanOrEqual,
             R"(t:min="2" t:max="2")",
             R"(t:min="3" t:max="2")"},
            {ExyokiOffice::MetadataSchematronComparisonOperator::GreaterThan,
             R"(t:min="3" t:max="2")",
             R"(t:min="2" t:max="2")"},
            {ExyokiOffice::MetadataSchematronComparisonOperator::GreaterThanOrEqual,
             R"(t:min="2" t:max="2")",
             R"(t:min="1" t:max="2")"},
        };

        for (const auto& item : cases)
        {
            ExyokiOffice::MetadataSchematronAttributeNumericAttributeComparisonConstraint comparison(
                Attr("min"),
                item.Operator,
                Attr("max"),
                "@t:min compare @t:max");
            CHECK(IsValid(comparison, *MakeAttributeElement("")));
            CHECK(IsValid(comparison, *MakeAttributeElement(R"(t:min="1")")));
            CHECK(IsValid(comparison, *MakeAttributeElement(R"(t:max="2")")));
            CHECK(IsValid(comparison, *MakeAttributeElement(item.ValidAttributes)));
            CHECK_FALSE(IsValid(comparison, *MakeAttributeElement(item.InvalidAttributes)));
            CHECK_FALSE(IsValid(comparison, *MakeAttributeElement(R"(t:min="bad" t:max="2")")));
            CHECK_FALSE(IsValid(comparison, *MakeAttributeElement(R"(t:min="1" t:max="bad")")));
        }

        ExyokiOffice::MetadataSchematronAttributeMutualExclusionConstraint exclusivePair(
            std::vector<ExyokiOffice::OpenXmlQualifiedName>{Attr("dn"), Attr("r")},
            "@t:dn and @t:r");
        CHECK(IsValid(exclusivePair, *MakeAttributeElement(R"(t:dn="a")")));
        CHECK(IsValid(exclusivePair, *MakeAttributeElement(R"(t:r="b")")));
        CHECK_FALSE(IsValid(exclusivePair, *MakeAttributeElement(R"(t:dn="a" t:r="b")")));
    }

    TEST_CASE("schematron conditional constraints validate generated conditions directly [unit] [metadata] [metadata-constraints]")
    {
        ExyokiOffice::MetadataSchematronAttributeConditionalPresenceConstraint requiredWhenType(
            Attr("operator"),
            Attr("type"),
            "cells",
            "(@t:operator and @t:type = cells) or @t:type != cells");
        CHECK(IsValid(requiredWhenType, *MakeAttributeElement("")));
        CHECK(IsValid(requiredWhenType, *MakeAttributeElement(R"(t:type="notCells")")));
        CHECK(IsValid(requiredWhenType, *MakeAttributeElement(R"(t:type="cells" t:operator="equal")")));
        CHECK_FALSE(IsValid(requiredWhenType, *MakeAttributeElement(R"(t:type="cells")")));

        ExyokiOffice::MetadataSchematronAttributeConditionalRequiredValueConstraint requiredValueWhenOle(
            Attr("name"),
            "StdDocumentName",
            Attr("ole"),
            "true",
            "(@t:name = StdDocumentName and @t:ole = true) or @t:ole != true");
        CHECK(IsValid(requiredValueWhenOle, *MakeAttributeElement("")));
        CHECK(IsValid(requiredValueWhenOle, *MakeAttributeElement(R"(t:ole="false")")));
        CHECK(IsValid(requiredValueWhenOle, *MakeAttributeElement(R"(t:ole="true" t:name="StdDocumentName")")));
        CHECK_FALSE(IsValid(requiredValueWhenOle, *MakeAttributeElement(R"(t:ole="true")")));
        CHECK_FALSE(IsValid(requiredValueWhenOle, *MakeAttributeElement(R"(t:ole="true" t:name="Other")")));

        ExyokiOffice::MetadataSchematronAttributeConditionalAllowedValuesConstraint conditionalAllowed(
            Attr("type"),
            std::vector<std::string>{"none", "all"},
            Attr("scope"),
            std::vector<std::string>{"data", "selection"},
            "conditional allowed values");
        CHECK(IsValid(conditionalAllowed, *MakeAttributeElement("")));
        CHECK(IsValid(conditionalAllowed, *MakeAttributeElement(R"(t:scope="field" t:type="other")")));
        CHECK(IsValid(conditionalAllowed, *MakeAttributeElement(R"(t:scope="data" t:type="none")")));
        CHECK_FALSE(IsValid(conditionalAllowed, *MakeAttributeElement(R"(t:scope="data")")));
        CHECK_FALSE(IsValid(conditionalAllowed, *MakeAttributeElement(R"(t:scope="selection" t:type="other")")));

        ExyokiOffice::MetadataSchematronAttributeConditionalForbiddenValuesConstraint conditionalForbidden(
            Attr("text"),
            Attr("type"),
            std::vector<std::string>{"beginsWith", "containsText"},
            "@t:text and @t:type != beginsWith and @t:type != containsText");
        CHECK(IsValid(conditionalForbidden, *MakeAttributeElement("")));
        CHECK(IsValid(conditionalForbidden, *MakeAttributeElement(R"(t:type="beginsWith")")));
        CHECK(IsValid(conditionalForbidden, *MakeAttributeElement(R"(t:text="abc" t:type="endsWith")")));
        CHECK_FALSE(IsValid(conditionalForbidden, *MakeAttributeElement(R"(t:text="abc" t:type="containsText")")));

        ExyokiOffice::MetadataSchematronAttributeConditionalPresenceAllowedValuesConstraint conditionalPresenceAllowed(
            Attr("dxfId"),
            Attr("sortBy"),
            std::vector<std::string>{"icon", "value"},
            "@t:dxfId and (@t:sortBy = icon or @t:sortBy = value)");
        CHECK(IsValid(conditionalPresenceAllowed, *MakeAttributeElement("")));
        CHECK(IsValid(conditionalPresenceAllowed, *MakeAttributeElement(R"(t:sortBy="cellColor")")));
        CHECK(IsValid(conditionalPresenceAllowed, *MakeAttributeElement(R"(t:dxfId="1" t:sortBy="icon")")));
        CHECK(IsValid(conditionalPresenceAllowed, *MakeAttributeElement(R"(t:dxfId="1" t:sortBy="value")")));
        CHECK_FALSE(IsValid(conditionalPresenceAllowed, *MakeAttributeElement(R"(t:dxfId="1")")));
        CHECK_FALSE(IsValid(conditionalPresenceAllowed, *MakeAttributeElement(R"(t:dxfId="1" t:sortBy="cellColor")")));
    }

} // TEST_SUITE("MetadataConstraintTests")
