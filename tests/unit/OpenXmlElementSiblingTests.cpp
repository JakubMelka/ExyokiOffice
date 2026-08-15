// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

// ---------------------------------------------------------------------------
// Pins sibling navigation on OpenXMLElement.
//
// Four of the traversal entry points answer questions about an element's
// neighbours rather than its children: FirstSibling and LastSibling reach the
// ends of the run the element belongs to, PreviousSibling and NextSibling step
// one place, and the typed GetNextSiblingOfType/GetPreviousSiblingOfType skip
// everything that is not the requested element type. The typed pair is what a
// caller reaches for to walk, say, the paragraphs of a body that also holds
// tables, so "skips the wrong types" and "stops at the end" are the contract,
// not an implementation detail.
// ---------------------------------------------------------------------------

#include "doctest.h"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Wordprocessing.hpp"
#include "ExyokiOffice/OpenXMLElement.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"

#include <memory>
#include <vector>

namespace
{
using ExyokiOffice::OpenXMLElement;
using ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Body;
using ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph;
using ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::SectionProperties;
using ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Table;
using ExyokiOffice::Word::WordDocumentEditor;

/** A body holding paragraph, table, paragraph, sectPr - in that order. */
struct MixedBody
{
    std::shared_ptr<WordDocumentEditor> Editor;
    std::shared_ptr<Body> Container;
    std::shared_ptr<Paragraph> FirstParagraph;
    std::shared_ptr<Table> MiddleTable;
    std::shared_ptr<Paragraph> SecondParagraph;
    std::shared_ptr<SectionProperties> Section;
};

MixedBody MakeMixedBody()
{
    MixedBody result;
    result.Editor = WordDocumentEditor::CreateNew();
    REQUIRE(result.Editor);
    auto mainPart = result.Editor->GetDocument()->GetMainDocumentPart();
    REQUIRE(mainPart);
    auto document = mainPart->GetTypedRootElement();
    REQUIRE(document);
    result.Container = document->GetFirstChildOfType<Body>();
    REQUIRE(result.Container);

    // A new document already carries body content; start from an empty body so
    // the positions below are the ones this test wrote.
    for (const auto& child : result.Container->Children())
    {
        REQUIRE(result.Container->RemoveChild(child));
    }
    REQUIRE(result.Container->Children().empty());

    result.FirstParagraph = result.Container->AppendChild<Paragraph>();
    result.MiddleTable = result.Container->AppendChild<Table>();
    result.SecondParagraph = result.Container->AppendChild<Paragraph>();
    result.Section = result.Container->AppendChild<SectionProperties>();
    REQUIRE(result.FirstParagraph);
    REQUIRE(result.MiddleTable);
    REQUIRE(result.SecondParagraph);
    REQUIRE(result.Section);
    return result;
}

} // namespace

TEST_CASE("OpenXMLElement: FirstSibling and LastSibling reach the ends of the run")
{
    const auto body = MakeMixedBody();

    // Asked from the middle, from an end, and from the element itself, the
    // answer is the same element: these name positions in the parent, not
    // offsets from the caller.
    for (const std::shared_ptr<OpenXMLElement>& from :
         std::vector<std::shared_ptr<OpenXMLElement>>{body.FirstParagraph, body.MiddleTable,
                                                      body.SecondParagraph, body.Section})
    {
        auto first = from->FirstSibling();
        auto last = from->LastSibling();
        REQUIRE(first);
        REQUIRE(last);
        CHECK(first->IsSameNode(*body.FirstParagraph));
        CHECK(last->IsSameNode(*body.Section));
    }
}

TEST_CASE("OpenXMLElement: PreviousSibling and NextSibling are each other's inverse")
{
    const auto body = MakeMixedBody();

    CHECK(body.FirstParagraph->PreviousSibling() == nullptr);
    CHECK(body.Section->NextSibling() == nullptr);

    auto forward = body.FirstParagraph->NextSibling();
    REQUIRE(forward);
    CHECK(forward->IsSameNode(*body.MiddleTable));

    auto backAgain = forward->PreviousSibling();
    REQUIRE(backAgain);
    CHECK(backAgain->IsSameNode(*body.FirstParagraph));

    // Walking the whole run forwards and then backwards must visit the same
    // four nodes in reverse, which is what makes either direction usable for
    // iteration.
    std::vector<ExyokiOffice::OpenXmlNodeHandle> forwards;
    for (auto current = body.FirstParagraph->FirstSibling(); current; current = current->NextSibling())
    {
        forwards.push_back(current->GetNodeHandle());
    }
    std::vector<ExyokiOffice::OpenXmlNodeHandle> backwards;
    for (auto current = body.Section->LastSibling(); current; current = current->PreviousSibling())
    {
        backwards.push_back(current->GetNodeHandle());
    }
    REQUIRE(forwards.size() == 4);
    REQUIRE(backwards.size() == forwards.size());
    for (std::size_t index = 0; index < forwards.size(); ++index)
    {
        CHECK(forwards[index] == backwards[backwards.size() - 1 - index]);
    }
}

TEST_CASE("OpenXMLElement: the typed sibling search skips elements of other types")
{
    const auto body = MakeMixedBody();

    // A table sits between the two paragraphs, so the untyped step and the
    // typed one disagree - which is the whole point of the typed overload.
    auto nextParagraph = body.FirstParagraph->GetNextSiblingOfType<Paragraph>();
    REQUIRE(nextParagraph);
    CHECK(nextParagraph->IsSameNode(*body.SecondParagraph));
    CHECK(body.FirstParagraph->NextSibling()->IsSameNode(*body.MiddleTable));

    auto previousParagraph = body.Section->GetPreviousSiblingOfType<Paragraph>();
    REQUIRE(previousParagraph);
    CHECK(previousParagraph->IsSameNode(*body.SecondParagraph));

    auto previousTable = body.SecondParagraph->GetPreviousSiblingOfType<Table>();
    REQUIRE(previousTable);
    CHECK(previousTable->IsSameNode(*body.MiddleTable));

    // Nothing of that type in the direction asked for is a null result, not the
    // nearest element of some other type.
    CHECK(body.SecondParagraph->GetNextSiblingOfType<Paragraph>() == nullptr);
    CHECK(body.FirstParagraph->GetPreviousSiblingOfType<Table>() == nullptr);
    CHECK(body.MiddleTable->GetNextSiblingOfType<Table>() == nullptr);
}

TEST_CASE("OpenXMLElement: sibling navigation on a parentless element stays inside itself")
{
    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor);
    auto mainPart = editor->GetDocument()->GetMainDocumentPart();
    REQUIRE(mainPart);

    // The part root has no parent element, so it is the only member of its own
    // run: both ends resolve to it and neither step leads anywhere.
    auto root = mainPart->GetTypedRootElement();
    REQUIRE(root);
    auto first = root->FirstSibling();
    auto last = root->LastSibling();
    REQUIRE(first);
    REQUIRE(last);
    CHECK(first->IsSameNode(*root));
    CHECK(last->IsSameNode(*root));
    CHECK(root->PreviousSibling() == nullptr);
    CHECK(root->NextSibling() == nullptr);

    // An unbound wrapper has no node to navigate from at all.
    const auto detached = std::make_shared<Paragraph>();
    REQUIRE(detached);
    CHECK(detached->FirstSibling() == nullptr);
    CHECK(detached->LastSibling() == nullptr);
    CHECK(detached->PreviousSibling() == nullptr);
    CHECK(detached->GetNextSiblingOfType<Paragraph>() == nullptr);
    CHECK(detached->GetPreviousSiblingOfType<Paragraph>() == nullptr);
}
