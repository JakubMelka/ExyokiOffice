// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

// ---------------------------------------------------------------------------
// Pins the structural DOM operations of OpenXMLElement: copy, move, replace and
// remove, plus the wrapper-invalidation contract they promise.
//
// The two rules under test are: a move inside one document relinks the node and
// therefore keeps every wrapper into the moved subtree usable, while anything
// that destroys a node resets the wrapper it was handed. Copies must also stay
// self-contained, which means carrying the namespace bindings the copied content
// depends on — the default namespace included.
// ---------------------------------------------------------------------------

#include "doctest.h"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Wordprocessing.hpp"
#include "ExyokiOffice/OpenXMLElement.hpp"
#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/OpenXmlPackagePart.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"
#include "zip/zip.h"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using ExyokiOffice::OpenXmlCloneDepth;
using ExyokiOffice::OpenXMLElement;
using ExyokiOffice::OpenXmlPlacement;
using ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Body;
using ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph;
using ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::ParagraphProperties;
using ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Run;
using ExyokiOffice::Word::WordDocumentEditor;

/** A loaded single-part package plus its root element, kept alive together. */
struct XmlFixture
{
    std::shared_ptr<ExyokiOffice::OpenXmlPackage> Package;
    std::shared_ptr<ExyokiOffice::OpenXmlPackagePart> Part;
    std::shared_ptr<OpenXMLElement> Root;
};

void AddZipEntry(zip_t* archive, const char* name, std::string_view content)
{
    REQUIRE(zip_entry_open(archive, name) == 0);
    CHECK(zip_entry_write(archive, content.data(), content.size()) == 0);
    zip_entry_close(archive);
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

/** Loads `xml` as the only XML part of an in-memory package. */
XmlFixture LoadXml(std::string_view xml)
{
    XmlFixture fixture;
    fixture.Package = std::make_shared<ExyokiOffice::OpenXmlPackage>();
    REQUIRE(fixture.Package->LoadFromMemory(BuildSingleXmlPartPackage(xml)));

    fixture.Part = fixture.Package->GetPartByUri("/custom.xml");
    REQUIRE(fixture.Part != nullptr);

    fixture.Root = fixture.Part->GetRootElement();
    REQUIRE(fixture.Root != nullptr);
    return fixture;
}

/** Returns the local names of `element`'s element children, in document order. */
std::vector<std::string> ChildNames(const std::shared_ptr<OpenXMLElement>& element)
{
    std::vector<std::string> names;
    for (const auto& child : element->Children())
    {
        names.emplace_back(child->QualifiedName().localName());
    }
    return names;
}

/** Creates an empty Word document and returns its body element. */
std::shared_ptr<Body> BodyOf(const std::shared_ptr<WordDocumentEditor>& editor)
{
    auto mainPart = editor->GetDocument()->GetMainDocumentPart();
    REQUIRE(mainPart != nullptr);
    auto root = mainPart->GetTypedRootElement();
    REQUIRE(root != nullptr);
    auto body = root->GetFirstChildOfType<Body>();
    REQUIRE(body != nullptr);
    return body;
}
} // namespace

TEST_CASE("OpenXMLElement: CopyInto honors the requested clone depth")
{
    auto source = LoadXml(R"(<?xml version="1.0" encoding="UTF-8"?>)"
                          R"(<t:root xmlns:t="urn:exyokioffice:test">)"
                          R"(<t:a id="7"><t:b/></t:a></t:root>)");
    auto target = LoadXml(R"(<?xml version="1.0" encoding="UTF-8"?>)"
                          R"(<t:target xmlns:t="urn:exyokioffice:test"/>)");

    auto sourceChild = source.Root->Children().front();
    REQUIRE(sourceChild != nullptr);

    const auto deepCopy = sourceChild->CopyInto(target.Root);
    REQUIRE(deepCopy != nullptr);
    CHECK(ChildNames(deepCopy) == std::vector<std::string>{"b"});

    const auto shallowCopy = sourceChild->CopyInto(target.Root, nullptr, OpenXmlCloneDepth::Shallow);
    REQUIRE(shallowCopy != nullptr);
    CHECK(shallowCopy->Children().empty());

    // Both copies keep the element's own attributes and its namespace.
    for (const auto& copy : {deepCopy, shallowCopy})
    {
        CHECK(copy->QualifiedName().localName() == "a");
        CHECK(copy->QualifiedName().namespaceUri() == "urn:exyokioffice:test");
        CHECK(copy->GetAttribute(ExyokiOffice::OpenXmlQualifiedName({}, "id")) == "7");
    }

    // The source is untouched.
    CHECK(ChildNames(source.Root) == std::vector<std::string>{"a"});
}

TEST_CASE("OpenXMLElement: CopyInto carries the default namespace binding across documents")
{
    // The copied element relies on an inherited xmlns="..." and the destination
    // declares a different default namespace.
    auto source = LoadXml(R"(<?xml version="1.0" encoding="UTF-8"?>)"
                          R"(<root xmlns="urn:exyokioffice:source"><child/></root>)");
    auto target = LoadXml(R"(<?xml version="1.0" encoding="UTF-8"?>)"
                          R"(<t:target xmlns:t="urn:exyokioffice:test" xmlns="urn:exyokioffice:other"/>)");

    auto child = source.Root->Children().front();
    REQUIRE(child != nullptr);
    REQUIRE(child->QualifiedName().namespaceUri() == "urn:exyokioffice:source");

    const auto copy = child->CopyInto(target.Root);
    REQUIRE(copy != nullptr);
    CHECK(copy->QualifiedName().namespaceUri() == "urn:exyokioffice:source");

    // Re-materializing from the destination tree proves the binding was stored,
    // not just reported by the wrapper the copy returned.
    auto reread = target.Root->Children().front();
    REQUIRE(reread != nullptr);
    CHECK(reread->QualifiedName().namespaceUri() == "urn:exyokioffice:source");
}

TEST_CASE("OpenXMLElement: CopyInto keeps unnamespaced content out of the destination default namespace")
{
    auto source = LoadXml(R"(<?xml version="1.0" encoding="UTF-8"?>)"
                          R"(<root><child/></root>)");
    auto target = LoadXml(R"(<?xml version="1.0" encoding="UTF-8"?>)"
                          R"(<t:target xmlns:t="urn:exyokioffice:test" xmlns="urn:exyokioffice:other"/>)");

    auto child = source.Root->Children().front();
    REQUIRE(child != nullptr);
    REQUIRE(child->QualifiedName().namespaceUri().empty());

    REQUIRE(child->CopyInto(target.Root) != nullptr);

    auto reread = target.Root->Children().front();
    REQUIRE(reread != nullptr);
    CHECK(reread->QualifiedName().namespaceUri().empty());
}

TEST_CASE("OpenXMLElement: CopyInto with SchemaOrdered corrects the requested position")
{
    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor);
    auto body = BodyOf(editor);

    auto paragraph = body->AppendChild<Paragraph>();
    REQUIRE(paragraph);
    REQUIRE(paragraph->AppendChild<Run>());

    // A stand-alone w:pPr that has to end up in front of the run.
    auto donor = body->AppendChild<Paragraph>();
    REQUIRE(donor);
    auto properties = donor->AppendChild<ParagraphProperties>();
    REQUIRE(properties);

    const auto placed = properties->CopyInto(paragraph, nullptr, OpenXmlCloneDepth::Deep,
                                             OpenXmlPlacement::SchemaOrdered);
    REQUIRE(placed != nullptr);
    CHECK(ChildNames(paragraph) == std::vector<std::string>{"pPr", "r"});

    // Exact placement appends instead, even though that breaks the content model.
    auto other = body->AppendChild<Paragraph>();
    REQUIRE(other);
    REQUIRE(other->AppendChild<Run>());
    REQUIRE(properties->CopyInto(other) != nullptr);
    CHECK(ChildNames(other) == std::vector<std::string>{"r", "pPr"});
}

TEST_CASE("OpenXMLElement: CopyAfter places the copy behind its anchor")
{
    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor);
    auto body = BodyOf(editor);

    auto first = body->AppendChild<Paragraph>();
    auto second = body->AppendChild<Paragraph>();
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(first->AppendChild<Run>());

    const auto copy = first->CopyAfter(first);
    REQUIRE(copy != nullptr);

    const auto children = body->Children();
    REQUIRE(children.size() == 3);
    CHECK(children[0]->IsSameNode(*first));
    CHECK(children[1]->IsSameNode(*copy));
    CHECK(children[2]->IsSameNode(*second));
    CHECK(ChildNames(copy) == std::vector<std::string>{"r"});
}

TEST_CASE("OpenXMLElement: MoveInto inside one document preserves node identity")
{
    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor);
    auto body = BodyOf(editor);

    auto first = body->AppendChild<Paragraph>();
    auto second = body->AppendChild<Paragraph>();
    REQUIRE(first);
    REQUIRE(second);
    auto run = second->AppendChild<Run>();
    REQUIRE(run);

    const auto moved = second->MoveInto(body, first);
    REQUIRE(moved != nullptr);

    // The node was relinked, not recreated: the pre-move wrappers - including the
    // one for a descendant - still view the very same nodes.
    CHECK_FALSE(second->IsNull());
    CHECK(second->IsSameNode(*moved));
    CHECK_FALSE(run->IsNull());
    CHECK(run->Parent()->IsSameNode(*second));

    const auto children = body->Children();
    REQUIRE(children.size() == 2);
    CHECK(children[0]->IsSameNode(*second));
    CHECK(children[1]->IsSameNode(*first));
}

TEST_CASE("OpenXMLElement: MoveInto across documents copies and clears the source wrapper")
{
    auto sourceEditor = WordDocumentEditor::CreateNew();
    auto targetEditor = WordDocumentEditor::CreateNew();
    REQUIRE(sourceEditor);
    REQUIRE(targetEditor);
    auto sourceBody = BodyOf(sourceEditor);
    auto targetBody = BodyOf(targetEditor);

    auto paragraph = sourceBody->AppendChild<Paragraph>();
    REQUIRE(paragraph);
    REQUIRE(paragraph->AppendChild<Run>());

    const auto moved = paragraph->MoveInto(targetBody);
    REQUIRE(moved != nullptr);
    CHECK(ChildNames(moved) == std::vector<std::string>{"r"});
    CHECK(moved->QualifiedName().localName() == "p");

    // Cross-document is copy plus destroy, so the source wrapper is reset.
    CHECK(paragraph->IsNull());
    CHECK(sourceBody->Elements<Paragraph>().empty());
    CHECK(targetBody->Elements<Paragraph>().size() == 1);
}

TEST_CASE("OpenXMLElement: MoveInto refuses to move an element into its own subtree")
{
    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor);
    auto body = BodyOf(editor);

    auto paragraph = body->AppendChild<Paragraph>();
    REQUIRE(paragraph);

    CHECK(body->MoveInto(paragraph) == nullptr);
    CHECK_FALSE(body->IsNull());
    CHECK(body->Elements<Paragraph>().size() == 1);
    CHECK(paragraph->Parent()->IsSameNode(*body));
}

TEST_CASE("OpenXMLElement: MoveAfter reorders siblings")
{
    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor);
    auto body = BodyOf(editor);

    auto first = body->AppendChild<Paragraph>();
    auto second = body->AppendChild<Paragraph>();
    auto third = body->AppendChild<Paragraph>();
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(third);

    REQUIRE(first->MoveAfter(third) != nullptr);

    const auto children = body->Children();
    REQUIRE(children.size() == 3);
    CHECK(children[0]->IsSameNode(*second));
    CHECK(children[1]->IsSameNode(*third));
    CHECK(children[2]->IsSameNode(*first));

    // Moving behind the element that already precedes it is a no-op.
    REQUIRE(first->MoveAfter(third) != nullptr);
    CHECK(body->Children()[2]->IsSameNode(*first));
}

TEST_CASE("OpenXMLElement: ReplaceWith takes over the position and clears the replaced wrapper")
{
    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor);
    auto body = BodyOf(editor);

    auto first = body->AppendChild<Paragraph>();
    auto second = body->AppendChild<Paragraph>();
    auto replacement = body->AppendChild<Paragraph>();
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(replacement);
    REQUIRE(replacement->AppendChild<Run>());

    const auto placed = first->ReplaceWith(replacement);
    REQUIRE(placed != nullptr);
    CHECK(placed->IsSameNode(*replacement));
    CHECK(first->IsNull());

    const auto children = body->Children();
    REQUIRE(children.size() == 2);
    CHECK(children[0]->IsSameNode(*replacement));
    CHECK(children[1]->IsSameNode(*second));
}

TEST_CASE("OpenXMLElement: ReplaceWith copies a replacement from another document")
{
    auto targetEditor = WordDocumentEditor::CreateNew();
    auto sourceEditor = WordDocumentEditor::CreateNew();
    REQUIRE(targetEditor);
    REQUIRE(sourceEditor);
    auto targetBody = BodyOf(targetEditor);
    auto sourceBody = BodyOf(sourceEditor);

    auto replaced = targetBody->AppendChild<Paragraph>();
    REQUIRE(replaced);
    auto foreign = sourceBody->AppendChild<Paragraph>();
    REQUIRE(foreign);
    REQUIRE(foreign->AppendChild<Run>());

    const auto placed = replaced->ReplaceWith(foreign);
    REQUIRE(placed != nullptr);
    CHECK(replaced->IsNull());
    CHECK(ChildNames(placed) == std::vector<std::string>{"r"});

    // A copy, so the donor document keeps its own paragraph.
    CHECK_FALSE(foreign->IsNull());
    CHECK(sourceBody->Elements<Paragraph>().size() == 1);
    CHECK(targetBody->Elements<Paragraph>().size() == 1);
}

TEST_CASE("OpenXMLElement: ReplaceWith rejects an ancestor and changes nothing")
{
    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor);
    auto body = BodyOf(editor);

    auto paragraph = body->AppendChild<Paragraph>();
    REQUIRE(paragraph);

    CHECK(paragraph->ReplaceWith(body) == nullptr);
    CHECK_FALSE(paragraph->IsNull());
    CHECK(paragraph->Parent()->IsSameNode(*body));
}

TEST_CASE("OpenXMLElement: ReplaceWithChildren promotes content in order and keeps child identity")
{
    auto fixture = LoadXml(R"(<?xml version="1.0" encoding="UTF-8"?>)"
                           R"(<t:root xmlns:t="urn:exyokioffice:test">)"
                           R"(<t:wrap><t:x/>kept text<t:y/></t:wrap><t:z/></t:root>)");

    auto wrap = fixture.Root->Children().front();
    REQUIRE(wrap != nullptr);
    const auto originalChildren = wrap->Children();
    REQUIRE(originalChildren.size() == 2);

    const auto promoted = wrap->ReplaceWithChildren();
    REQUIRE(promoted.size() == 2);
    CHECK(promoted[0]->IsSameNode(*originalChildren[0]));
    CHECK(promoted[1]->IsSameNode(*originalChildren[1]));

    // The wrapper element is gone, its content took its place, and the text node
    // travelled with the elements.
    CHECK(wrap->IsNull());
    CHECK(ChildNames(fixture.Root) == std::vector<std::string>{"x", "y", "z"});
    CHECK(fixture.Part->GetXmlString().find("kept text") != std::string::npos);
}

TEST_CASE("OpenXMLElement: Remove and RemoveChild clear the wrapper they are handed")
{
    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor);
    auto body = BodyOf(editor);

    auto removedBySelf = body->AppendChild<Paragraph>();
    auto removedByParent = body->AppendChild<Paragraph>();
    REQUIRE(removedBySelf);
    REQUIRE(removedByParent);

    REQUIRE(removedBySelf->Remove());
    CHECK(removedBySelf->IsNull());
    CHECK_FALSE(removedBySelf->Remove());

    REQUIRE(body->RemoveChild(removedByParent));
    CHECK(removedByParent->IsNull());
    CHECK_FALSE(body->RemoveChild(removedByParent));

    CHECK(body->Elements<Paragraph>().empty());
}

TEST_CASE("OpenXMLElement: RemoveAllChildren empties the element but keeps it usable")
{
    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor);
    auto body = BodyOf(editor);

    REQUIRE(body->AppendChild<Paragraph>());
    REQUIRE(body->AppendChild<Paragraph>());

    CHECK(body->RemoveAllChildren());
    CHECK_FALSE(body->IsNull());
    CHECK(body->Children().empty());

    // Still a live element that accepts new content.
    CHECK(body->AppendChild<Paragraph>() != nullptr);
}

TEST_CASE("OpenXMLElement: InsertChildAfter places a new child behind its anchor")
{
    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor);
    auto body = BodyOf(editor);

    auto first = body->AppendChild<Paragraph>();
    auto last = body->AppendChild<Paragraph>();
    REQUIRE(first);
    REQUIRE(last);

    auto middle = body->InsertChildAfter<Paragraph>(first);
    REQUIRE(middle);

    auto children = body->Children();
    REQUIRE(children.size() == 3);
    CHECK(children[0]->IsSameNode(*first));
    CHECK(children[1]->IsSameNode(*middle));
    CHECK(children[2]->IsSameNode(*last));

    // A last-child anchor appends, and so does a foreign one.
    auto appended = body->InsertChildAfter<Paragraph>(last);
    REQUIRE(appended);
    CHECK(body->Children().back()->IsSameNode(*appended));

    auto afterForeign = body->InsertChildAfter<Paragraph>(middle->AppendChild<Run>());
    REQUIRE(afterForeign);
    CHECK(body->Children().back()->IsSameNode(*afterForeign));
}

TEST_CASE("OpenXMLElement: InsertChildAfter respects the content model, the raw variant does not")
{
    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor);
    auto body = BodyOf(editor);

    auto schemaAware = body->AppendChild<Paragraph>();
    REQUIRE(schemaAware);
    auto run = schemaAware->AppendChild<Run>();
    REQUIRE(run);

    // w:pPr must precede the runs, so the requested position is corrected.
    REQUIRE(schemaAware->InsertChildAfter<ParagraphProperties>(run) != nullptr);
    CHECK(ChildNames(schemaAware) == std::vector<std::string>{"pPr", "r"});

    auto raw = body->AppendChild<Paragraph>();
    REQUIRE(raw);
    auto rawRun = raw->AppendChild<Run>();
    REQUIRE(rawRun);

    REQUIRE(raw->InsertChildAfterRaw<ParagraphProperties>(rawRun) != nullptr);
    CHECK(ChildNames(raw) == std::vector<std::string>{"r", "pPr"});
}
