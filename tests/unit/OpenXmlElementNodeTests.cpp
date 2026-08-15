// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

// ---------------------------------------------------------------------------
// Pins the node-handle semantics of OpenXMLElement.
//
// An element wrapper stores nothing but an opaque handle to the pugixml node it
// views. A wrapper that was never bound to a node is null; wrappers are minted
// fresh on every traversal, so two wrappers over one node are distinct objects
// that must still compare equal by node identity.
// ---------------------------------------------------------------------------

#include "doctest.h"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Wordprocessing.hpp"
#include "ExyokiOffice/OpenXMLElement.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"

#include <memory>

namespace
{
using ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Body;
using ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph;
using ExyokiOffice::Word::WordDocumentEditor;
} // namespace

TEST_CASE("OpenXMLElement: an unbound wrapper is null")
{
    const auto detached = std::make_shared<Paragraph>();
    REQUIRE(detached);
    CHECK(detached->IsNull());
    CHECK_FALSE(detached->GetNodeHandle());

    // A null wrapper answers every query without touching a node.
    CHECK(detached->Parent() == nullptr);
    CHECK(detached->Children().empty());
    CHECK(detached->NextSibling() == nullptr);
    CHECK_FALSE(detached->IsSameNode(*detached));
}

TEST_CASE("OpenXMLElement: independently materialized wrappers share node identity")
{
    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor);
    auto mainPart = editor->GetDocument()->GetMainDocumentPart();
    REQUIRE(mainPart);

    // Every accessor mints a fresh wrapper; the two below are different objects
    // over one and the same XML node.
    const auto first = mainPart->GetTypedRootElement();
    const auto second = mainPart->GetTypedRootElement();
    REQUIRE(first);
    REQUIRE(second);
    CHECK(first != second);
    CHECK_FALSE(first->IsNull());
    CHECK(first->GetNodeHandle() == second->GetNodeHandle());
    CHECK(first->IsSameNode(*second));
    CHECK(first->IsSameNode(second));

    auto body = first->GetFirstChildOfType<Body>();
    REQUIRE(body);
    CHECK_FALSE(body->IsSameNode(*first));
    CHECK(body->Parent()->IsSameNode(*first));

    auto paragraph = body->AppendChild<Paragraph>();
    REQUIRE(paragraph);
    CHECK_FALSE(paragraph->IsNull());
    CHECK(paragraph->Parent()->IsSameNode(*body));
    CHECK(body->GetFirstChildOfType<Paragraph>()->IsSameNode(*paragraph));

    // Removing the child resets the wrapper it was handed, so a stale wrapper
    // reports itself as null instead of pointing at a destroyed node.
    REQUIRE(body->RemoveChild(paragraph));
    CHECK(paragraph->IsNull());
    CHECK(body->Elements<Paragraph>().empty());
}
