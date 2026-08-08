// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Presentation.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Drawing.hpp"
#include "ExyokiOffice/OpenXmlPackageValidator.hpp"
#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

using ExyokiOffice::PowerPoint::PowerPointDocumentEditor;
using ExyokiOffice::PowerPoint::PresentationPoint;
using ExyokiOffice::PowerPoint::PresentationShapeTransform;
using ExyokiOffice::PowerPoint::PresentationSize;
namespace Presentation = ExyokiOffice::DocumentFormat::OpenXml::Presentation;

namespace
{
std::string ShapeName(const std::shared_ptr<ExyokiOffice::PowerPoint::PresentationShape>& shape)
{
    return shape->GetElement()
        ->Descendants<ExyokiOffice::DocumentFormat::OpenXml::Presentation::NonVisualDrawingProperties>()[0]
        ->GetName()
        .ToString();
}
} // namespace

TEST_SUITE("PowerPointShapeTreeTests")
{
    TEST_CASE("shape tree writes non-visual properties in the PresentationML namespace [unit] [powerpoint] [shapes]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto slide = editor->AddSlide();
        REQUIRE(slide != nullptr);
        auto tree = slide->ShapeTree();
        REQUIRE(tree != nullptr);
        REQUIRE(tree->AddShape("Shape") != nullptr);
        REQUIRE(tree->AddConnector("Connector") != nullptr);

        // p:nvSpPr wants p:cNvPr. The DrawingML a:cNvPr shares the local name and
        // the C++ class name, so getting this wrong is invisible until the schema
        // is consulted - and it silently breaks id allocation, because the id scan
        // then finds none of the properties it wrote.
        const auto xml = slide->GetPart()->GetXmlString();
        CHECK(xml.find("<a:cNvPr") == std::string::npos);
        CHECK(xml.find("<p:cNvPr") != std::string::npos);
        CHECK(xml.find("<a:cNvGrpSpPr") == std::string::npos);

        const auto result = ExyokiOffice::OpenXmlPackageValidator(ExyokiOffice::OpenXmlDomValidationSettings{})
                                .Validate(*editor->GetDocument());
        for (const auto& issue : result.Issues())
        {
            CHECK(issue.Id != ExyokiOffice::ValidationErrorId::ParticleConstraintViolation);
        }

        // Ids stay unique because the allocator can see the properties it writes.
        REQUIRE(tree->Count() == 2);
        const auto first = tree->Get(0)->GetElement()->Descendants<Presentation::NonVisualDrawingProperties>();
        const auto second = tree->Get(1)->GetElement()->Descendants<Presentation::NonVisualDrawingProperties>();
        REQUIRE_FALSE(first.empty());
        REQUIRE_FALSE(second.empty());
        CHECK(first.front()->GetId().ValueOr(0) != second.front()->GetId().ValueOr(0));
    }

    TEST_CASE("shape tree adds enumerates removes and round trips shapes in XML order [unit] [powerpoint] [shapes]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto slide = editor->AddSlide();
        REQUIRE(slide != nullptr);
        auto tree = slide->ShapeTree();
        REQUIRE(tree != nullptr);
        CHECK(tree->Count() == 0);
        auto back = tree->AddShape("Back");
        auto middle = tree->AddShape("Middle");
        auto front = tree->AddShape();
        REQUIRE(back != nullptr);
        REQUIRE(middle != nullptr);
        REQUIRE(front != nullptr);
        REQUIRE(tree->Count() == 3);
        CHECK(tree->Get(0)->GetElement()->IsSameNode(*back->GetElement()));
        CHECK(tree->Get(1)->GetElement()->IsSameNode(*middle->GetElement()));
        CHECK(tree->Get(3) == nullptr);
        CHECK(std::dynamic_pointer_cast<Presentation::Shape>(tree->Get(2)->GetElement()) != nullptr);

        REQUIRE(tree->Remove(1));
        CHECK(tree->Count() == 2);
        CHECK_FALSE(middle->Remove());
        CHECK_FALSE(tree->Remove(5));

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);
        REQUIRE(reopened->GetSlide(0)->ShapeTree() != nullptr);
        CHECK(reopened->GetSlide(0)->ShapeTree()->Count() == 2);
        const auto validation = ExyokiOffice::OpenXmlPackageValidator().Validate(*reopened->GetDocument());
        CHECK(validation.IsValid());
    }

    TEST_CASE("z-order operations match direct XML child order and reject boundaries [unit] [powerpoint] [shapes]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto tree = editor->AddSlide()->ShapeTree();
        REQUIRE(tree->AddShape("A") != nullptr);
        REQUIRE(tree->AddShape("B") != nullptr);
        REQUIRE(tree->AddShape("C") != nullptr);
        auto nameAt = [&](ExyokiOffice::Size index)
        {
            auto shape = std::dynamic_pointer_cast<Presentation::Shape>(tree->Get(index)->GetElement());
            return shape->Descendants<ExyokiOffice::DocumentFormat::OpenXml::Presentation::NonVisualDrawingProperties>()[0]
                ->GetName()
                .ToString();
        };
        REQUIRE(tree->BringForward(0));
        CHECK(nameAt(0) == "B");
        CHECK(nameAt(1) == "A");
        REQUIRE(tree->BringToFront(0));
        CHECK(nameAt(2) == "B");
        REQUIRE(tree->SendToBack(2));
        CHECK(nameAt(0) == "B");
        REQUIRE(tree->SendBackward(2));
        CHECK(nameAt(1) == "C");
        CHECK_FALSE(tree->SendBackward(0));
        CHECK_FALSE(tree->BringForward(2));
        CHECK_FALSE(tree->Move(3, 0));

        auto xmlShapes = tree->Get(0)->GetElement()->Parent()->Elements<Presentation::Shape>();
        REQUIRE(xmlShapes.size() == 3);
        CHECK(xmlShapes[0]->IsSameNode(*tree->Get(0)->GetElement()));
        CHECK(xmlShapes[2]->IsSameNode(*tree->Get(2)->GetElement()));
    }

    TEST_CASE("grouping preserves selected XML order supports nested content and is atomic on bad indices [unit] [powerpoint] [shapes]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto tree = editor->AddSlide()->ShapeTree();
        REQUIRE(tree->AddShape("A") != nullptr);
        REQUIRE(tree->AddShape("B") != nullptr);
        REQUIRE(tree->AddShape("C") != nullptr);
        REQUIRE(tree->AddShape("D") != nullptr);
        CHECK(tree->Group({1}) == nullptr);
        CHECK(tree->Group({1, 1}) == nullptr);
        CHECK(tree->Group({0, 9}) == nullptr);
        CHECK(tree->Count() == 4);

        auto group = tree->Group({3, 1});
        REQUIRE(group != nullptr);
        CHECK(group->IsGroup());
        REQUIRE(tree->Count() == 3);
        CHECK(tree->Get(1)->IsGroup());
        auto children = group->Children();
        REQUIRE(children.size() == 2);
        auto childName = [](const auto& child)
        {
            return child->GetElement()
                ->template Descendants<ExyokiOffice::DocumentFormat::OpenXml::Presentation::NonVisualDrawingProperties>()[0]
                ->GetName()
                .ToString();
        };
        CHECK(childName(children[0]) == "B");
        CHECK(childName(children[1]) == "D");
        CHECK_FALSE(children[0]->IsGroup());

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);
        auto reopenedTree = reopened->GetSlide(0)->ShapeTree();
        REQUIRE(reopenedTree->Count() == 3);
        REQUIRE(reopenedTree->Get(1)->IsGroup());
        CHECK(reopenedTree->Get(1)->Children().size() == 2);
        CHECK(reopenedTree->Get(1)->Remove());
        CHECK(reopenedTree->Count() == 2);
    }

    TEST_CASE("ungroup promotes children in place preserving surrounding z-order [unit] [powerpoint] [shapes]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto tree = editor->AddSlide()->ShapeTree();
        REQUIRE(tree->AddShape("A") != nullptr);
        REQUIRE(tree->AddShape("B") != nullptr);
        REQUIRE(tree->AddShape("C") != nullptr);
        REQUIRE(tree->AddShape("D") != nullptr);

        auto group = tree->Group({1, 2}); // B and C
        REQUIRE(group != nullptr);
        REQUIRE(tree->Count() == 3); // A, group(B,C), D

        REQUIRE(tree->Ungroup(1));
        REQUIRE(tree->Count() == 4);
        CHECK(ShapeName(tree->Get(0)) == "A");
        CHECK(ShapeName(tree->Get(1)) == "B");
        CHECK(ShapeName(tree->Get(2)) == "C");
        CHECK(ShapeName(tree->Get(3)) == "D");
        CHECK_FALSE(tree->Get(1)->IsGroup());

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);
        auto reopenedTree = reopened->GetSlide(0)->ShapeTree();
        REQUIRE(reopenedTree->Count() == 4);
        CHECK(ShapeName(reopenedTree->Get(2)) == "C");
    }

    TEST_CASE("ungroup re-projects child coordinates into the parent tree [unit] [powerpoint] [shapes]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto tree = editor->AddSlide()->ShapeTree();
        REQUIRE(tree->AddShape("A") != nullptr);
        REQUIRE(tree->AddShape("B") != nullptr);

        auto group = tree->Group({0, 1});
        REQUIRE(group != nullptr);

        // Group occupies [1000000,1000000]+[2000000,2000000] on the slide, while its
        // child coordinate system is [0,0]+[1000000,1000000]; the scale factor is 2.
        PresentationShapeTransform groupTransform{
            .Position = {1000000, 1000000},
            .Size = {2000000, 2000000},
            .GroupChildPosition = PresentationPoint{0, 0},
            .GroupChildSize = PresentationSize{1000000, 1000000}};
        REQUIRE(group->SetTransform(groupTransform));

        auto children = group->Children();
        REQUIRE(children.size() == 2);
        REQUIRE(children[0]->SetTransform({.Position = {0, 0}, .Size = {500000, 500000}}));
        REQUIRE(children[1]->SetTransform({.Position = {500000, 500000}, .Size = {250000, 250000}}));

        REQUIRE(tree->Ungroup(0));
        REQUIRE(tree->Count() == 2);

        auto first = tree->Get(0)->GetTransform();
        REQUIRE(first);
        CHECK(first->Position == PresentationPoint{1000000, 1000000});
        CHECK(first->Size == PresentationSize{1000000, 1000000});

        auto second = tree->Get(1)->GetTransform();
        REQUIRE(second);
        CHECK(second->Position == PresentationPoint{2000000, 2000000});
        CHECK(second->Size == PresentationSize{500000, 500000});
    }

    TEST_CASE("ungroup rejects invalid indices and non-group shapes without changes [unit] [powerpoint] [shapes]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto tree = editor->AddSlide()->ShapeTree();
        REQUIRE(tree->AddShape("A") != nullptr);
        REQUIRE(tree->AddShape("B") != nullptr);
        auto group = tree->Group({0, 1});
        REQUIRE(group != nullptr);
        REQUIRE(tree->Count() == 1);

        CHECK_FALSE(tree->Ungroup(5)); // out of range
        CHECK(tree->Count() == 1);

        tree->AddShape("C"); // a plain shape at index 1
        REQUIRE(tree->Count() == 2);
        CHECK_FALSE(tree->Ungroup(1)); // not a group
        CHECK(tree->Count() == 2);
        CHECK(tree->Get(0)->IsGroup());
    }

    TEST_CASE("shape tree handles malformed slides safely and enumerates all supported hosts [unit] [powerpoint] [shapes]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto slide = editor->AddSlide();
        slide->GetPart()->SetXmlString(
            "<p:sld xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\"/> ");
        CHECK(slide->ShapeTree() == nullptr);

        slide->GetPart()->SetXmlString(
            "<p:sld xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\" "
            "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\"><p:cSld><p:spTree>"
            "<p:nvGrpSpPr/><p:grpSpPr/><p:pic/><p:graphicFrame/><p:cxnSp/><p:contentPart/>"
            "</p:spTree></p:cSld></p:sld>");
        auto tree = slide->ShapeTree();
        REQUIRE(tree != nullptr);
        REQUIRE(tree->Count() == 4);
        CHECK(std::dynamic_pointer_cast<Presentation::Picture>(tree->Get(0)->GetElement()) != nullptr);
        CHECK(std::dynamic_pointer_cast<Presentation::GraphicFrame>(tree->Get(1)->GetElement()) != nullptr);
        CHECK(std::dynamic_pointer_cast<Presentation::ConnectionShape>(tree->Get(2)->GetElement()) != nullptr);
        CHECK(std::dynamic_pointer_cast<Presentation::ContentPart>(tree->Get(3)->GetElement()) != nullptr);
    }
} // namespace
