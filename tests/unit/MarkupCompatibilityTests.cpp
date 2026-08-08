// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

// ---------------------------------------------------------------------------
// Pins the hand-written markup compatibility layer: mc:AlternateContent and its
// branches materialize as typed elements even though no schema metadata declares
// them, and the mc attributes read back as token lists.
// ---------------------------------------------------------------------------

#include "doctest.h"

#include "ExyokiOffice/MarkupCompatibility.hpp"
#include "ExyokiOffice/OpenXmlDomValidator.hpp"
#include "ExyokiOffice/OpenXMLElement.hpp"
#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/OpenXmlPackagePart.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"
#include "zip/zip.h"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using ExyokiOffice::AlternateContent;
using ExyokiOffice::AlternateContentChoice;
using ExyokiOffice::AlternateContentFallback;
using ExyokiOffice::MarkupCompatibilityAttributes;
using ExyokiOffice::MarkupCompatibilityNames;
using ExyokiOffice::OpenXMLElement;

constexpr std::string_view kWordNs = "http://schemas.openxmlformats.org/wordprocessingml/2006/main";

void AddZipEntry(zip_t* archive, const char* name, std::string_view content)
{
    REQUIRE(zip_entry_open(archive, name) == 0);
    CHECK(zip_entry_write(archive, content.data(), content.size()) == 0);
    zip_entry_close(archive);
}

/** Loads `xml` as the only XML part of an in-memory package and returns its root. */
std::shared_ptr<OpenXMLElement> LoadRootElement(std::string_view xml)
{
    static std::vector<std::shared_ptr<ExyokiOffice::OpenXmlPackage>> packages;

    auto* archive = zip_stream_open(nullptr, 0, ZIP_DEFAULT_COMPRESSION_LEVEL, 'w');
    REQUIRE(archive != nullptr);
    AddZipEntry(archive,
                "[Content_Types].xml",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="xml" ContentType="application/xml"/>
</Types>)");
    AddZipEntry(archive, "custom.xml", xml);

    void* rawBuffer = nullptr;
    ExyokiOffice::Size rawSize = 0;
    REQUIRE(zip_stream_copy(archive, &rawBuffer, &rawSize) > 0);
    zip_stream_close(archive);
    REQUIRE(rawBuffer != nullptr);
    const auto* bytes = static_cast<const ExyokiOffice::UInt8*>(rawBuffer);
    std::vector<ExyokiOffice::Byte> buffer(bytes, bytes + rawSize);
    std::free(rawBuffer);

    auto package = std::make_shared<ExyokiOffice::OpenXmlPackage>();
    REQUIRE(package->LoadFromMemory(buffer));
    auto part = package->GetPartByUri("/custom.xml");
    REQUIRE(part != nullptr);
    auto root = part->GetRootElement();
    REQUIRE(root != nullptr);
    packages.push_back(std::move(package));
    return root;
}

/** The namespace declarations every processing fixture shares. */
constexpr std::string_view kFixtureNamespaces =
    R"( xmlns:mc="http://schemas.openxmlformats.org/markup-compatibility/2006")"
    R"( xmlns:w14="http://schemas.microsoft.com/office/word/2010/wordml")"
    R"( xmlns:w15="http://schemas.microsoft.com/office/word/2012/wordml")"
    R"( xmlns:v="urn:vendor:extension")";

/** Wraps `content` in a w:body that declares the fixture namespaces. */
std::shared_ptr<OpenXMLElement> LoadBody(std::string_view content, std::string_view bodyAttributes = {})
{
    std::string xml = R"(<w:body xmlns:w=")";
    xml += kWordNs;
    xml += R"(")";
    xml += kFixtureNamespaces;
    xml += bodyAttributes;
    xml += ">";
    xml += content;
    xml += "</w:body>";
    return LoadRootElement(xml);
}

/** A body holding one mc:AlternateContent with the given branches. */
std::shared_ptr<OpenXMLElement> LoadAlternateContent(std::string_view branches)
{
    std::string content = "<mc:AlternateContent>";
    content += branches;
    content += "</mc:AlternateContent>";
    return LoadBody(content);
}

/** Local names of an element's element children, in document order. */
std::vector<std::string> ChildNames(const std::shared_ptr<OpenXMLElement>& element)
{
    std::vector<std::string> names;
    for (const auto& child : element->Children())
    {
        names.emplace_back(child->QualifiedName().localName());
    }
    return names;
}

/** Runs the processor over `root` for a target generation, collecting diagnostics. */
struct ProcessOutcome
{
    bool Succeeded = false;
    ExyokiOffice::ValidationResult Diagnostics;

    [[nodiscard]] bool HasIssue(ExyokiOffice::ValidationErrorId id) const
    {
        const auto& issues = Diagnostics.Issues();
        return std::any_of(issues.begin(), issues.end(),
                           [id](const auto& issue)
                           { return issue.Id == id; });
    }
};

/** One mc:AlternateContent choosing between a w14 branch and a plain fallback. */
constexpr std::string_view kAlternateContentMarkup =
    R"(<mc:AlternateContent>)"
    R"(<mc:Choice Requires="w14"><w14:modern/></mc:Choice>)"
    R"(<mc:Fallback><w:classic/></mc:Fallback>)"
    R"(</mc:AlternateContent>)";

/** Replaces a part's XML with a root element holding the alternate content. */
void WriteAlternateContentPart(const std::shared_ptr<ExyokiOffice::OpenXmlPackagePart>& part,
                               std::string_view rootName,
                               std::string_view rootAttributes = {},
                               bool wrapInBody = true)
{
    std::string xml = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)";
    xml += "<";
    xml += rootName;
    xml += R"( xmlns:w=")";
    xml += kWordNs;
    xml += R"(")";
    xml += kFixtureNamespaces;
    xml += rootAttributes;
    xml += ">";
    xml += wrapInBody ? "<w:body>" : "";
    xml += kAlternateContentMarkup;
    xml += wrapInBody ? "</w:body>" : "";
    xml += "</";
    xml += rootName;
    xml += ">";
    part->SetXmlString(xml);
}

/** Builds a .docx whose body holds one mc:AlternateContent, as raw package bytes. */
std::vector<ExyokiOffice::Byte> BuildDocumentWithAlternateContent(std::string_view bodyAttributes = {})
{
    auto editor = ExyokiOffice::Word::WordDocumentEditor::CreateNew();
    REQUIRE(editor != nullptr);
    auto mainPart = editor->GetDocument()->GetMainDocumentPart();
    REQUIRE(mainPart != nullptr);

    // Written as raw XML: the branches use elements no generated class covers, and
    // the attribute under test names prefixes that must be declared alongside it.
    WriteAlternateContentPart(mainPart, "w:document", bodyAttributes);
    return editor->SaveToMemory();
}

ProcessOutcome Process(const std::shared_ptr<OpenXMLElement>& root,
                       ExyokiOffice::OpenXml::FileFormatVersions target =
                           ExyokiOffice::OpenXml::FileFormatVersions::Office2007,
                       ExyokiOffice::MarkupCompatibilityProcessMode mode =
                           ExyokiOffice::MarkupCompatibilityProcessMode::ProcessAllParts)
{
    ProcessOutcome outcome;
    ExyokiOffice::MarkupCompatibilityProcessSettings settings;
    settings.ProcessMode = mode;
    settings.TargetFileFormatVersions = target;
    ExyokiOffice::MarkupCompatibilityProcessor processor(settings, &outcome.Diagnostics);
    outcome.Succeeded = processor.Process(root);
    return outcome;
}
} // namespace

TEST_SUITE("MarkupCompatibilityTests")
{
    TEST_CASE("markup compatibility elements materialize as typed elements [unit] [markup-compatibility]")
    {
        auto body = LoadAlternateContent(
            R"(<mc:Choice Requires="v"><v:shape/></mc:Choice>)"
            R"(<mc:Fallback><w:p/></mc:Fallback>)");

        // Without the hand-written layer these would all be GenericOpenXmlElement,
        // because the generated factory only knows the schema vocabularies.
        auto alternate = body->GetFirstChildOfType<AlternateContent>();
        REQUIRE(alternate != nullptr);
        CHECK(alternate->QualifiedName() ==
              ExyokiOffice::OpenXmlQualifiedName(ExyokiOffice::kMarkupCompatibilityNamespace,
                                                 "AlternateContent"));

        const auto choices = alternate->Choices();
        REQUIRE(choices.size() == 1);
        CHECK(choices.front()->GetRequires().ToString() == "v");
        CHECK(choices.front()->RequiredPrefixes() == std::vector<std::string>{"v"});

        auto fallback = alternate->Fallback();
        REQUIRE(fallback != nullptr);
        CHECK(fallback->Children().size() == 1);
    }

    TEST_CASE("a choice can require several namespaces [unit] [markup-compatibility]")
    {
        auto body = LoadAlternateContent(R"(<mc:Choice Requires="  v   w  "><v:shape/></mc:Choice>)");
        auto alternate = body->GetFirstChildOfType<AlternateContent>();
        REQUIRE(alternate != nullptr);
        REQUIRE(alternate->Choices().size() == 1);

        // Whitespace runs separate the prefixes and never produce empty entries.
        CHECK(alternate->Choices().front()->RequiredPrefixes() == std::vector<std::string>{"v", "w"});
        CHECK(alternate->Fallback() == nullptr);
    }

    TEST_CASE("markup compatibility elements can be built through the typed API [unit] [markup-compatibility]")
    {
        auto body = LoadRootElement(R"(<w:body xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"/>)");

        auto alternate = body->AppendChildRaw<AlternateContent>();
        REQUIRE(alternate != nullptr);
        auto choice = alternate->AppendChildRaw<AlternateContentChoice>();
        REQUIRE(choice != nullptr);
        choice->SetRequires(ExyokiOffice::StringValue("v"));
        REQUIRE(alternate->AppendChildRaw<AlternateContentFallback>() != nullptr);

        // Reading the tree back finds the same typed elements, so the writer and
        // the reader agree on the namespace.
        auto reread = body->GetFirstChildOfType<AlternateContent>();
        REQUIRE(reread != nullptr);
        REQUIRE(reread->Choices().size() == 1);
        CHECK(reread->Choices().front()->RequiredPrefixes() == std::vector<std::string>{"v"});
        CHECK(reread->Fallback() != nullptr);
    }

    TEST_CASE("markup compatibility attributes read back as token lists [unit] [markup-compatibility]")
    {
        auto body = LoadRootElement(
            R"(<w:body xmlns:w=")" + std::string(kWordNs) +
            R"(" xmlns:mc="http://schemas.openxmlformats.org/markup-compatibility/2006")"
            R"( mc:Ignorable="v w14" mc:ProcessContent="v:wrap" mc:MustUnderstand="w14")"
            R"( mc:PreserveElements="*" mc:PreserveAttributes="v:style"/>)");

        const auto attributes = MarkupCompatibilityAttributes::Read(*body);
        CHECK_FALSE(attributes.IsEmpty());
        CHECK(attributes.Ignorable == std::vector<std::string>{"v", "w14"});
        CHECK(attributes.ProcessContent == std::vector<std::string>{"v:wrap"});
        CHECK(attributes.MustUnderstand == std::vector<std::string>{"w14"});
        CHECK(attributes.PreserveElements == std::vector<std::string>{"*"});
        CHECK(attributes.PreserveAttributes == std::vector<std::string>{"v:style"});
    }

    TEST_CASE("an element without markup compatibility attributes reads empty [unit] [markup-compatibility]")
    {
        auto body = LoadRootElement(R"(<w:body xmlns:w=")" + std::string(kWordNs) + R"("/>)");
        const auto attributes = MarkupCompatibilityAttributes::Read(*body);
        CHECK(attributes.IsEmpty());
        CHECK(attributes.Ignorable.empty());
    }

    TEST_CASE("alternate content is not validated against a content model [unit] [markup-compatibility]")
    {
        auto body = LoadAlternateContent(
            R"(<mc:Choice Requires="v"><v:shape/></mc:Choice><mc:Fallback><w:p/></mc:Fallback>)");
        auto alternate = body->GetFirstChildOfType<AlternateContent>();
        REQUIRE(alternate != nullptr);

        // The branches hold whole subtrees of foreign vocabularies, so there is no
        // content model to judge them by; the validator must stay silent about the
        // mc elements themselves.
        const ExyokiOffice::OpenXmlDomValidator validator;
        CHECK(validator.Validate(*alternate).IsValid());
    }

    TEST_CASE("the first understood choice wins [unit] [markup-compatibility]")
    {
        // w14 belongs to Office 2010, so at an Office 2013 target both choices are
        // understood and declaration order decides.
        auto body = LoadAlternateContent(
            R"(<mc:Choice Requires="w14"><w14:first/></mc:Choice>)"
            R"(<mc:Choice Requires="w"><w:second/></mc:Choice>)"
            R"(<mc:Fallback><w:third/></mc:Fallback>)");

        const auto outcome = Process(body, ExyokiOffice::OpenXml::FileFormatVersions::Office2013);
        CHECK(outcome.Succeeded);
        CHECK(ChildNames(body) == std::vector<std::string>{"first"});
        CHECK(body->GetFirstChildOfType<AlternateContent>() == nullptr);
    }

    TEST_CASE("a choice the target does not understand is skipped [unit] [markup-compatibility]")
    {
        auto body = LoadAlternateContent(
            R"(<mc:Choice Requires="w14"><w14:modern/></mc:Choice>)"
            R"(<mc:Choice Requires="w"><w:classic/></mc:Choice>)"
            R"(<mc:Fallback><w:plain/></mc:Fallback>)");

        // Office 2007 predates w14, so the second choice is the first one that applies.
        const auto outcome = Process(body, ExyokiOffice::OpenXml::FileFormatVersions::Office2007);
        CHECK(outcome.Succeeded);
        CHECK(ChildNames(body) == std::vector<std::string>{"classic"});
    }

    TEST_CASE("the fallback is used when no choice applies [unit] [markup-compatibility]")
    {
        auto body = LoadAlternateContent(
            R"(<mc:Choice Requires="w14"><w14:modern/></mc:Choice>)"
            R"(<mc:Choice Requires="w15"><w15:newer/></mc:Choice>)"
            R"(<mc:Fallback><w:plain/><w:extra/></mc:Fallback>)");

        const auto outcome = Process(body, ExyokiOffice::OpenXml::FileFormatVersions::Office2007);
        CHECK(outcome.Succeeded);

        // The whole content of the fallback is promoted, in order.
        CHECK(ChildNames(body) == std::vector<std::string>{"plain", "extra"});
    }

    TEST_CASE("alternate content without an applicable branch disappears [unit] [markup-compatibility]")
    {
        auto body = LoadAlternateContent(R"(<mc:Choice Requires="w14"><w14:modern/></mc:Choice>)");

        const auto outcome = Process(body, ExyokiOffice::OpenXml::FileFormatVersions::Office2007);
        CHECK(outcome.Succeeded);
        CHECK(body->Children().empty());
    }

    TEST_CASE("a choice requiring several namespaces needs all of them [unit] [markup-compatibility]")
    {
        const std::string branches =
            R"(<mc:Choice Requires="w w14"><w14:both/></mc:Choice>)"
            R"(<mc:Fallback><w:plain/></mc:Fallback>)";

        // w alone is understood at Office 2007, w14 is not, so the pair is not either.
        auto older = LoadAlternateContent(branches);
        CHECK(Process(older, ExyokiOffice::OpenXml::FileFormatVersions::Office2007).Succeeded);
        CHECK(ChildNames(older) == std::vector<std::string>{"plain"});

        auto newer = LoadAlternateContent(branches);
        CHECK(Process(newer, ExyokiOffice::OpenXml::FileFormatVersions::Office2010).Succeeded);
        CHECK(ChildNames(newer) == std::vector<std::string>{"both"});
    }

    TEST_CASE("a vendor namespace is never understood [unit] [markup-compatibility]")
    {
        auto body = LoadAlternateContent(
            R"(<mc:Choice Requires="v"><v:shape/></mc:Choice><mc:Fallback><w:plain/></mc:Fallback>)");

        // Even the newest target cannot understand a namespace outside Open XML.
        const auto outcome = Process(body, ExyokiOffice::OpenXml::FileFormatVersions::Microsoft365);
        CHECK(outcome.Succeeded);
        CHECK(ChildNames(body) == std::vector<std::string>{"plain"});
    }

    TEST_CASE("promoted content keeps its position and node identity [unit] [markup-compatibility]")
    {
        auto body = LoadBody(
            R"(<w:before/>)"
            R"(<mc:AlternateContent><mc:Choice Requires="w"><w:chosen/></mc:Choice></mc:AlternateContent>)"
            R"(<w:after/>)");

        auto chosen = body->Children()[1]->Children().front()->Children().front();
        REQUIRE(chosen != nullptr);
        REQUIRE(chosen->QualifiedName().localName() == "chosen");

        CHECK(Process(body).Succeeded);

        // The branch content lands exactly where the alternate content stood, and it
        // was moved rather than copied, so the wrapper taken beforehand still views it.
        CHECK(ChildNames(body) == std::vector<std::string>{"before", "chosen", "after"});
        CHECK_FALSE(chosen->IsNull());
        CHECK(body->Children()[1]->IsSameNode(*chosen));
    }

    TEST_CASE("alternate content nested in a chosen branch is resolved too [unit] [markup-compatibility]")
    {
        auto body = LoadAlternateContent(
            R"(<mc:Choice Requires="w"><w:outer>)"
            R"(<mc:AlternateContent><mc:Choice Requires="w14"><w14:inner/></mc:Choice>)"
            R"(<mc:Fallback><w:innerPlain/></mc:Fallback></mc:AlternateContent>)"
            R"(</w:outer></mc:Choice>)");

        CHECK(Process(body, ExyokiOffice::OpenXml::FileFormatVersions::Office2007).Succeeded);
        REQUIRE(ChildNames(body) == std::vector<std::string>{"outer"});
        CHECK(ChildNames(body->Children().front()) == std::vector<std::string>{"innerPlain"});
    }

    TEST_CASE("alternate content nested in a fallback is resolved too [unit] [markup-compatibility]")
    {
        auto body = LoadAlternateContent(
            R"(<mc:Choice Requires="w15"><w15:newer/></mc:Choice>)"
            R"(<mc:Fallback><mc:AlternateContent>)"
            R"(<mc:Choice Requires="w14"><w14:middle/></mc:Choice>)"
            R"(<mc:Fallback><w:oldest/></mc:Fallback></mc:AlternateContent></mc:Fallback>)");

        CHECK(Process(body, ExyokiOffice::OpenXml::FileFormatVersions::Office2010).Succeeded);
        CHECK(ChildNames(body) == std::vector<std::string>{"middle"});
    }

    TEST_CASE("ignorable content in an unknown namespace is dropped [unit] [markup-compatibility]")
    {
        auto body = LoadBody(R"(<w:keep/><v:drop/>)", R"( mc:Ignorable="v")");

        CHECK(Process(body).Succeeded);
        CHECK(ChildNames(body) == std::vector<std::string>{"keep"});
    }

    TEST_CASE("ignorable content the target understands is kept [unit] [markup-compatibility]")
    {
        const std::string content = R"(<w:keep/><w14:maybe/>)";

        // Declaring a namespace ignorable does not mean discarding it: a consumer
        // that understands it keeps the content.
        auto understood = LoadBody(content, R"( mc:Ignorable="w14")");
        CHECK(Process(understood, ExyokiOffice::OpenXml::FileFormatVersions::Office2010).Succeeded);
        CHECK(ChildNames(understood) == std::vector<std::string>{"keep", "maybe"});

        auto notUnderstood = LoadBody(content, R"( mc:Ignorable="w14")");
        CHECK(Process(notUnderstood, ExyokiOffice::OpenXml::FileFormatVersions::Office2007).Succeeded);
        CHECK(ChildNames(notUnderstood) == std::vector<std::string>{"keep"});
    }

    TEST_CASE("content in an unknown namespace that is not ignorable stays [unit] [markup-compatibility]")
    {
        // Without mc:Ignorable there is no permission to drop anything.
        auto body = LoadBody(R"(<w:keep/><v:unknown/>)");
        CHECK(Process(body).Succeeded);
        CHECK(ChildNames(body) == std::vector<std::string>{"keep", "unknown"});
    }

    TEST_CASE("ProcessContent promotes the children of a dropped element [unit] [markup-compatibility]")
    {
        auto body = LoadBody(R"(<v:wrap><w:inside/><w:alsoInside/></v:wrap><w:after/>)",
                             R"( mc:Ignorable="v" mc:ProcessContent="v:wrap")");

        CHECK(Process(body).Succeeded);
        CHECK(ChildNames(body) == std::vector<std::string>{"inside", "alsoInside", "after"});
    }

    TEST_CASE("PreserveElements keeps ignorable content verbatim [unit] [markup-compatibility]")
    {
        auto byName = LoadBody(R"(<v:keepMe><v:child/></v:keepMe><v:dropMe/>)",
                               R"( mc:Ignorable="v" mc:PreserveElements="v:keepMe")");
        CHECK(Process(byName).Succeeded);
        REQUIRE(ChildNames(byName) == std::vector<std::string>{"keepMe"});

        // Preserved means untouched: the processor does not descend into it either.
        CHECK(ChildNames(byName->Children().front()) == std::vector<std::string>{"child"});

        auto byWildcard = LoadBody(R"(<v:one/><v:two/>)", R"( mc:Ignorable="v" mc:PreserveElements="*")");
        CHECK(Process(byWildcard).Succeeded);
        CHECK(ChildNames(byWildcard) == std::vector<std::string>{"one", "two"});
    }

    TEST_CASE("ignorable attributes are dropped unless preserved [unit] [markup-compatibility]")
    {
        const ExyokiOffice::OpenXmlQualifiedName vendorStyle("urn:vendor:extension", "style");
        const ExyokiOffice::OpenXmlQualifiedName vendorHint("urn:vendor:extension", "hint");

        auto dropped = LoadBody(R"(<w:p v:style="fancy" v:hint="none"/>)", R"( mc:Ignorable="v")");
        CHECK(Process(dropped).Succeeded);
        auto paragraph = dropped->Children().front();
        CHECK_FALSE(paragraph->HasAttribute(vendorStyle));
        CHECK_FALSE(paragraph->HasAttribute(vendorHint));

        auto preserved = LoadBody(R"(<w:p v:style="fancy" v:hint="none"/>)",
                                  R"( mc:Ignorable="v" mc:PreserveAttributes="v:style")");
        CHECK(Process(preserved).Succeeded);
        paragraph = preserved->Children().front();
        CHECK(paragraph->GetAttribute(vendorStyle) == "fancy");
        CHECK_FALSE(paragraph->HasAttribute(vendorHint));

        auto wildcard = LoadBody(R"(<w:p v:style="fancy" v:hint="none"/>)",
                                 R"( mc:Ignorable="v" mc:PreserveAttributes="*")");
        CHECK(Process(wildcard).Succeeded);
        paragraph = wildcard->Children().front();
        CHECK(paragraph->GetAttribute(vendorStyle) == "fancy");
        CHECK(paragraph->GetAttribute(vendorHint) == "none");
    }

    TEST_CASE("ignorable rules apply to descendants of the declaring element [unit] [markup-compatibility]")
    {
        auto body = LoadBody(R"(<w:p><w:r><v:deep/></w:r></w:p>)", R"( mc:Ignorable="v")");

        CHECK(Process(body).Succeeded);
        auto run = body->Children().front()->Children().front();
        REQUIRE(run != nullptr);
        CHECK(run->Children().empty());
    }

    TEST_CASE("MustUnderstand stops processing when the namespace is unsupported [unit] [markup-compatibility]")
    {
        auto body = LoadBody(R"(<w:p/>)", R"( mc:MustUnderstand="w14")");

        const auto refused = Process(body, ExyokiOffice::OpenXml::FileFormatVersions::Office2007);
        CHECK_FALSE(refused.Succeeded);
        CHECK(refused.HasIssue(
            ExyokiOffice::ValidationErrorId::MarkupCompatibilityMustUnderstandUnsupported));

        auto accepted = LoadBody(R"(<w:p/>)", R"( mc:MustUnderstand="w14")");
        const auto allowed = Process(accepted, ExyokiOffice::OpenXml::FileFormatVersions::Office2010);
        CHECK(allowed.Succeeded);
        CHECK(allowed.Diagnostics.IsValid());
    }

    TEST_CASE("consumed markup compatibility attributes are removed [unit] [markup-compatibility]")
    {
        auto body = LoadBody(R"(<w:p/>)",
                             R"( mc:Ignorable="v" mc:ProcessContent="v:wrap" mc:PreserveElements="*")");

        CHECK(Process(body).Succeeded);

        // The rules have been applied, so the tree must not keep claiming they still
        // need applying.
        CHECK_FALSE(body->HasAttribute(MarkupCompatibilityNames::Ignorable()));
        CHECK_FALSE(body->HasAttribute(MarkupCompatibilityNames::ProcessContent()));
        CHECK_FALSE(body->HasAttribute(MarkupCompatibilityNames::PreserveElements()));
        CHECK(MarkupCompatibilityAttributes::Read(*body).IsEmpty());
    }

    TEST_CASE("a choice without Requires is reported and skipped [unit] [markup-compatibility]")
    {
        auto body = LoadAlternateContent(
            R"(<mc:Choice><w:broken/></mc:Choice><mc:Fallback><w:plain/></mc:Fallback>)");

        const auto outcome = Process(body);
        CHECK(outcome.Succeeded);
        CHECK(outcome.HasIssue(
            ExyokiOffice::ValidationErrorId::MarkupCompatibilityMalformedAlternateContent));
        CHECK(ChildNames(body) == std::vector<std::string>{"plain"});
    }

    TEST_CASE("foreign children of alternate content are reported [unit] [markup-compatibility]")
    {
        auto body = LoadAlternateContent(R"(<w:stray/><mc:Fallback><w:plain/></mc:Fallback>)");

        const auto outcome = Process(body);
        CHECK(outcome.Succeeded);
        CHECK(outcome.HasIssue(
            ExyokiOffice::ValidationErrorId::MarkupCompatibilityMalformedAlternateContent));

        // The stray element is not a branch, so it goes with the alternate content.
        CHECK(ChildNames(body) == std::vector<std::string>{"plain"});
    }

    TEST_CASE("an undeclared prefix in Requires is reported [unit] [markup-compatibility]")
    {
        auto body = LoadAlternateContent(
            R"(<mc:Choice Requires="nope"><w:broken/></mc:Choice><mc:Fallback><w:plain/></mc:Fallback>)");

        const auto outcome = Process(body);
        CHECK(outcome.Succeeded);
        CHECK(outcome.HasIssue(ExyokiOffice::ValidationErrorId::MarkupCompatibilityUnresolvablePrefix));
        CHECK(ChildNames(body) == std::vector<std::string>{"plain"});
    }

    TEST_CASE("processing without a diagnostic sink still works [unit] [markup-compatibility]")
    {
        auto body = LoadAlternateContent(R"(<mc:Choice><w:broken/></mc:Choice>)");

        ExyokiOffice::MarkupCompatibilityProcessSettings settings;
        settings.TargetFileFormatVersions = ExyokiOffice::OpenXml::FileFormatVersions::Office2007;
        ExyokiOffice::MarkupCompatibilityProcessor processor(settings);
        CHECK(processor.Process(body));
        CHECK(body->Children().empty());
    }

    TEST_CASE("IsUnderstoodNamespace follows the target generation [unit] [markup-compatibility]")
    {
        ExyokiOffice::MarkupCompatibilityProcessSettings settings;
        settings.TargetFileFormatVersions = ExyokiOffice::OpenXml::FileFormatVersions::Office2007;
        const ExyokiOffice::MarkupCompatibilityProcessor office2007(settings);

        settings.TargetFileFormatVersions = ExyokiOffice::OpenXml::FileFormatVersions::Office2013;
        const ExyokiOffice::MarkupCompatibilityProcessor office2013(settings);

        CHECK(office2007.IsUnderstoodNamespace(kWordNs));
        CHECK(office2013.IsUnderstoodNamespace(kWordNs));

        constexpr std::string_view w14 = "http://schemas.microsoft.com/office/word/2010/wordml";
        CHECK_FALSE(office2007.IsUnderstoodNamespace(w14));
        CHECK(office2013.IsUnderstoodNamespace(w14));

        CHECK_FALSE(office2013.IsUnderstoodNamespace("urn:vendor:extension"));

        // The markup compatibility namespace is always understood: it is what the
        // processor itself implements.
        CHECK(office2007.IsUnderstoodNamespace(ExyokiOffice::kMarkupCompatibilityNamespace));
    }

    TEST_CASE("package processing honors the selected mode [unit] [markup-compatibility]")
    {
        // Two parts, each with alternate content, related to one another so that both
        // walks can reach them.
        auto editor = ExyokiOffice::Word::WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto document = editor->GetDocument();
        auto mainPart = document->GetMainDocumentPart();
        REQUIRE(mainPart != nullptr);

        WriteAlternateContentPart(mainPart, "w:document");

        auto stylesPart = mainPart->AddStyleDefinitionsPart();
        REQUIRE(stylesPart != nullptr);
        WriteAlternateContentPart(stylesPart, "w:styles", {}, false);

        ExyokiOffice::MarkupCompatibilityProcessSettings settings;
        settings.TargetFileFormatVersions = ExyokiOffice::OpenXml::FileFormatVersions::Office2007;

        SUBCASE("NoProcess leaves every part untouched")
        {
            settings.ProcessMode = ExyokiOffice::MarkupCompatibilityProcessMode::NoProcess;
            CHECK(ExyokiOffice::ProcessMarkupCompatibility(*document, mainPart, settings));
            CHECK(mainPart->GetXmlString().find("AlternateContent") != std::string::npos);
            CHECK(stylesPart->GetXmlString().find("AlternateContent") != std::string::npos);
        }

        SUBCASE("ProcessAllParts reaches the related part as well")
        {
            settings.ProcessMode = ExyokiOffice::MarkupCompatibilityProcessMode::ProcessAllParts;
            CHECK(ExyokiOffice::ProcessMarkupCompatibility(*document, mainPart, settings));
            CHECK(mainPart->GetXmlString().find("AlternateContent") == std::string::npos);
            CHECK(stylesPart->GetXmlString().find("AlternateContent") == std::string::npos);
            CHECK(mainPart->GetXmlString().find("<w:classic") != std::string::npos);
        }

        SUBCASE("ProcessLoadedPartsOnly reaches the main part and what it references")
        {
            settings.ProcessMode = ExyokiOffice::MarkupCompatibilityProcessMode::ProcessLoadedPartsOnly;
            CHECK(ExyokiOffice::ProcessMarkupCompatibility(*document, mainPart, settings));

            // The styles part hangs directly off the main part, so it is in range.
            CHECK(mainPart->GetXmlString().find("AlternateContent") == std::string::npos);
            CHECK(stylesPart->GetXmlString().find("AlternateContent") == std::string::npos);
        }
    }

    TEST_CASE("OpenSettings decides whether markup compatibility is resolved [unit] [markup-compatibility]")
    {
        const auto bytes = BuildDocumentWithAlternateContent();

        ExyokiOffice::Packaging::OpenSettings untouched;
        auto raw = ExyokiOffice::Word::WordDocumentEditor::Open(bytes, untouched);
        REQUIRE(raw != nullptr);

        // The default really is "change nothing": the branches are still there.
        const auto rawXml = raw->GetDocument()->GetMainDocumentPart()->GetXmlString();
        CHECK(rawXml.find("AlternateContent") != std::string::npos);
        CHECK(rawXml.find("<w14:modern") != std::string::npos);
        CHECK(rawXml.find("<w:classic") != std::string::npos);

        ExyokiOffice::Packaging::OpenSettings resolving;
        resolving.MarkupCompatibility.ProcessMode =
            ExyokiOffice::MarkupCompatibilityProcessMode::ProcessAllParts;
        resolving.MarkupCompatibility.TargetFileFormatVersions =
            ExyokiOffice::OpenXml::FileFormatVersions::Office2007;
        auto processed = ExyokiOffice::Word::WordDocumentEditor::Open(bytes, resolving);
        REQUIRE(processed != nullptr);

        const auto processedXml = processed->GetDocument()->GetMainDocumentPart()->GetXmlString();
        CHECK(processedXml.find("AlternateContent") == std::string::npos);
        CHECK(processedXml.find("<w14:modern") == std::string::npos);
        CHECK(processedXml.find("<w:classic") != std::string::npos);

        // A newer target takes the other branch from the very same bytes.
        resolving.MarkupCompatibility.TargetFileFormatVersions =
            ExyokiOffice::OpenXml::FileFormatVersions::Office2013;
        auto modern = ExyokiOffice::Word::WordDocumentEditor::Open(bytes, resolving);
        REQUIRE(modern != nullptr);

        const auto modernXml = modern->GetDocument()->GetMainDocumentPart()->GetXmlString();
        CHECK(modernXml.find("<w14:modern") != std::string::npos);
        CHECK(modernXml.find("<w:classic") == std::string::npos);
    }

    TEST_CASE("processed documents survive a save and reopen [unit] [markup-compatibility]")
    {
        ExyokiOffice::Packaging::OpenSettings resolving;
        resolving.MarkupCompatibility.ProcessMode =
            ExyokiOffice::MarkupCompatibilityProcessMode::ProcessAllParts;
        resolving.MarkupCompatibility.TargetFileFormatVersions =
            ExyokiOffice::OpenXml::FileFormatVersions::Office2007;

        auto processed =
            ExyokiOffice::Word::WordDocumentEditor::Open(BuildDocumentWithAlternateContent(), resolving);
        REQUIRE(processed != nullptr);

        // Saving writes the resolved tree, which is the documented lossy part of the
        // deal: reopening finds the chosen branch and no compatibility markup.
        auto reopened = ExyokiOffice::Word::WordDocumentEditor::Open(processed->SaveToMemory());
        REQUIRE(reopened != nullptr);
        const auto xml = reopened->GetDocument()->GetMainDocumentPart()->GetXmlString();
        CHECK(xml.find("AlternateContent") == std::string::npos);
        CHECK(xml.find("<w:classic") != std::string::npos);
    }

    TEST_CASE("Open fails when the document demands an unsupported namespace [unit] [markup-compatibility]")
    {
        const auto bytes = BuildDocumentWithAlternateContent(R"( mc:MustUnderstand="w15")");

        ExyokiOffice::Packaging::OpenSettings resolving;
        resolving.MarkupCompatibility.ProcessMode =
            ExyokiOffice::MarkupCompatibilityProcessMode::ProcessAllParts;
        resolving.MarkupCompatibility.TargetFileFormatVersions =
            ExyokiOffice::OpenXml::FileFormatVersions::Office2007;

        ExyokiOffice::ValidationResult diagnostics;
        resolving.ValidationDiagnostics = &diagnostics;
        CHECK(ExyokiOffice::Word::WordDocumentEditor::Open(bytes, resolving) == nullptr);
        CHECK(diagnostics.HasErrors());

        // The same bytes open cleanly for a target that does cover the namespace, and
        // for the default mode that never looks.
        resolving.MarkupCompatibility.TargetFileFormatVersions =
            ExyokiOffice::OpenXml::FileFormatVersions::Office2013;
        CHECK(ExyokiOffice::Word::WordDocumentEditor::Open(bytes, resolving) != nullptr);
        CHECK(ExyokiOffice::Word::WordDocumentEditor::Open(bytes) != nullptr);
    }

    TEST_CASE("markup compatibility names are stable [unit] [markup-compatibility]")
    {
        CHECK(MarkupCompatibilityNames::AlternateContent().namespaceUri() ==
              ExyokiOffice::kMarkupCompatibilityNamespace);
        CHECK(MarkupCompatibilityNames::Choice().localName() == "Choice");
        CHECK(MarkupCompatibilityNames::Fallback().localName() == "Fallback");

        // Requires is declared on mc:Choice itself and is therefore unprefixed,
        // unlike the attributes that annotate foreign elements.
        CHECK(MarkupCompatibilityNames::Requires().namespaceUri().empty());
        CHECK(MarkupCompatibilityNames::Ignorable().namespaceUri() ==
              ExyokiOffice::kMarkupCompatibilityNamespace);
    }
}
