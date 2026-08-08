// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Tools/DocumentModelIO.hpp"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Drawing.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Presentation.hpp"
#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"
#include "ExyokiOffice/Tools/PackageInspector.hpp"
#include "ExyokiOffice/Tools/PackageLimits.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <cmath>

namespace ExyokiOffice::Tools
{

namespace
{

namespace P = ExyokiOffice::PowerPoint;
namespace DrawingEnums = ExyokiOffice::DocumentFormat::OpenXml::Drawing;
using PresentationPlaceholderValues = ExyokiOffice::DocumentFormat::OpenXml::Presentation::PlaceholderValues;

void Warn(std::vector<ToolDiagnostic>& diagnostics, std::string message, std::string context = {})
{
    diagnostics.push_back(ToolDiagnostic{ToolSeverity::Warning, std::move(message), std::move(context)});
}

struct PlaceholderTokenEntry
{
    PresentationPlaceholderValues::Value Value;
    const char* Token;
};

constexpr PlaceholderTokenEntry kPlaceholderTokens[] = {
    {PresentationPlaceholderValues::Title, "title"},
    {PresentationPlaceholderValues::Body, "body"},
    {PresentationPlaceholderValues::CenteredTitle, "ctrTitle"},
    {PresentationPlaceholderValues::SubTitle, "subTitle"},
    {PresentationPlaceholderValues::DateAndTime, "dt"},
    {PresentationPlaceholderValues::SlideNumber, "sldNum"},
    {PresentationPlaceholderValues::Footer, "ftr"},
    {PresentationPlaceholderValues::Header, "hdr"},
    {PresentationPlaceholderValues::Object, "body"},
    {PresentationPlaceholderValues::Chart, "chart"},
    {PresentationPlaceholderValues::Table, "tbl"},
    {PresentationPlaceholderValues::ClipArt, "clipArt"},
    {PresentationPlaceholderValues::Diagram, "dgm"},
    {PresentationPlaceholderValues::Media, "media"},
    {PresentationPlaceholderValues::SlideImage, "sldImg"},
    {PresentationPlaceholderValues::Picture, "pic"},
};

std::string PlaceholderToken(PresentationPlaceholderValues::Value value)
{
    for (const auto& entry : kPlaceholderTokens)
    {
        if (entry.Value == value)
        {
            return entry.Token;
        }
    }
    return "body";
}

bool IsTitleToken(const std::string& token)
{
    return token == "title" || token == "ctrTitle";
}

/// Reads the shape's non-visual name (`p:cNvPr/@name`). The element is typed
/// as the shared DrawingML CT_NonVisualDrawingProps.
std::string ShapeName(const P::PresentationShape& shape)
{
    const auto element = shape.GetElement();
    if (!element)
    {
        return {};
    }
    const auto properties =
        element->Descendants<ExyokiOffice::DocumentFormat::OpenXml::Presentation::NonVisualDrawingProperties>();
    if (properties.empty() || !properties.front()->GetName().IsDefined())
    {
        return {};
    }
    return properties.front()->GetName().ToString();
}

Int64 ToEmu(const MeasuringUnits& value)
{
    return static_cast<Int64>(std::llround(value.ToEmu().GetValue()));
}

PptTransform TransformToModel(const P::PresentationShapeTransform& transform)
{
    PptTransform model;
    model.Present = true;
    model.X = ToEmu(transform.Position.X);
    model.Y = ToEmu(transform.Position.Y);
    model.Cx = ToEmu(transform.Size.Width);
    model.Cy = ToEmu(transform.Size.Height);
    return model;
}

P::PresentationShapeTransform TransformFromModel(const PptTransform& model)
{
    P::PresentationShapeTransform transform;
    transform.Position = P::PresentationPoint(model.X, model.Y);
    transform.Size = P::PresentationSize(model.Cx, model.Cy);
    return transform;
}

P::PresentationShapeTransform DefaultTransform(Int64 xEmu, Int64 yEmu, Int64 cxEmu,
                                               Int64 cyEmu)
{
    P::PresentationShapeTransform transform;
    transform.Position = P::PresentationPoint(xEmu, yEmu);
    transform.Size = P::PresentationSize(cxEmu, cyEmu);
    return transform;
}

// --- Text frames ------------------------------------------------------------

PptTextFrame FrameToModel(const P::PresentationTextFrame& frame)
{
    PptTextFrame model;
    for (const auto& paragraph : frame.Paragraphs)
    {
        PptParagraph modelParagraph;
        modelParagraph.Level = static_cast<int>(paragraph.Level);
        for (const auto& run : paragraph.Runs)
        {
            PptRun modelRun;
            modelRun.Text = run.Text;
            modelRun.Bold = run.Bold;
            modelRun.Italic = run.Italic;
            modelRun.Underline = run.Underline != DrawingEnums::TextUnderlineValues::None &&
                                 run.Underline != DrawingEnums::TextUnderlineValues::NotDefinedEnumValue;
            if (run.FontSize)
            {
                modelRun.FontSizePt = run.FontSize->ToPt().GetValue();
            }
            if (run.FontColor)
            {
                modelRun.Color = run.FontColor->ToHexString();
            }
            if (run.Hyperlink)
            {
                modelRun.Hyperlink = *run.Hyperlink;
            }
            modelParagraph.Runs.push_back(std::move(modelRun));
        }
        model.Paragraphs.push_back(std::move(modelParagraph));
    }
    return model;
}

P::PresentationTextFrame FrameFromModel(const PptTextFrame& model)
{
    P::PresentationTextFrame frame;
    for (const auto& modelParagraph : model.Paragraphs)
    {
        P::PresentationTextParagraph paragraph;
        paragraph.Level = modelParagraph.Level;
        for (const auto& modelRun : modelParagraph.Runs)
        {
            P::PresentationTextRun run;
            run.Text = modelRun.Text;
            run.Bold = modelRun.Bold;
            run.Italic = modelRun.Italic;
            if (modelRun.Underline)
            {
                run.Underline = DrawingEnums::TextUnderlineValues::Single;
            }
            if (modelRun.FontSizePt > 0.0)
            {
                run.FontSize = MeasuringUnits(modelRun.FontSizePt, MeasurementUnit::Point);
            }
            if (!modelRun.Color.empty())
            {
                if (const auto color = Color::FromHexString(modelRun.Color); color && !color->IsAuto())
                {
                    run.FontColor = *color;
                }
            }
            if (!modelRun.Hyperlink.empty())
            {
                run.Hyperlink = modelRun.Hyperlink;
            }
            paragraph.Runs.push_back(std::move(run));
        }
        frame.Paragraphs.push_back(std::move(paragraph));
    }
    return frame;
}

std::string FramePlainText(const PptTextFrame& frame)
{
    std::string text;
    for (const auto& paragraph : frame.Paragraphs)
    {
        if (!text.empty())
        {
            text += '\n';
        }
        for (const auto& run : paragraph.Runs)
        {
            text += run.Text;
        }
    }
    return text;
}

// --- Tables -----------------------------------------------------------------

PptTableModel TableToModel(const P::PresentationTableData& table)
{
    PptTableModel model;
    for (const auto& row : table.Rows)
    {
        std::vector<PptTableCell> modelRow;
        for (const auto& cell : row.Cells)
        {
            PptTableCell modelCell;
            PptParagraph paragraph;
            PptRun run;
            run.Text = cell.Text;
            paragraph.Runs.push_back(std::move(run));
            modelCell.Text.Paragraphs.push_back(std::move(paragraph));
            modelRow.push_back(std::move(modelCell));
        }
        model.Rows.push_back(std::move(modelRow));
    }

    for (const auto& merge : table.Merges)
    {
        for (Size row = merge.Row; row < merge.Row + merge.RowSpan && row < model.Rows.size(); ++row)
        {
            for (Size column = merge.Column;
                 column < merge.Column + merge.ColumnSpan && column < model.Rows[row].size(); ++column)
            {
                auto& cell = model.Rows[row][column];
                if (row == merge.Row && column == merge.Column)
                {
                    cell.RowSpan = static_cast<int>(merge.RowSpan);
                    cell.ColSpan = static_cast<int>(merge.ColumnSpan);
                }
                else
                {
                    cell.Covered = true;
                }
            }
        }
    }
    return model;
}

P::PresentationTableData TableFromModel(const PptTableModel& model,
                                        const P::PresentationShapeTransform& transform)
{
    P::PresentationTableData table;
    Size columns = 0;
    for (const auto& row : model.Rows)
    {
        columns = std::max(columns, row.size());
    }
    if (columns == 0)
    {
        return table;
    }

    const Real totalWidthEmu = 8229600.0; // 9 inches
    for (Size i = 0; i < columns; ++i)
    {
        table.ColumnWidths.emplace_back(totalWidthEmu / static_cast<Real>(columns), MeasurementUnit::Emu);
    }

    for (const auto& modelRow : model.Rows)
    {
        P::PresentationTableRow row;
        row.Height = MeasuringUnits(0.4, MeasurementUnit::Inch);
        for (Size column = 0; column < columns; ++column)
        {
            P::PresentationTableCell cell;
            if (column < modelRow.size())
            {
                cell.Text = FramePlainText(modelRow[column].Text);
            }
            row.Cells.push_back(std::move(cell));
        }
        table.Rows.push_back(std::move(row));
    }

    for (Size rowIndex = 0; rowIndex < model.Rows.size(); ++rowIndex)
    {
        for (Size columnIndex = 0; columnIndex < model.Rows[rowIndex].size(); ++columnIndex)
        {
            const auto& cell = model.Rows[rowIndex][columnIndex];
            if (!cell.Covered && (cell.RowSpan > 1 || cell.ColSpan > 1))
            {
                P::PresentationTableMerge merge;
                merge.Row = rowIndex;
                merge.Column = columnIndex;
                merge.RowSpan = static_cast<Size>(cell.RowSpan);
                merge.ColumnSpan = static_cast<Size>(cell.ColSpan);
                table.Merges.push_back(merge);
            }
        }
    }

    table.Transform = transform;
    return table;
}

// --- Reader -----------------------------------------------------------------

class PowerPointModelReader
{
public:
    PowerPointModelReader(P::PowerPointDocumentEditor& editor, const ModelReadOptions& options,
                          DocumentModel& model, std::vector<ToolDiagnostic>& diagnostics)
        : m_editor(editor), m_options(options), m_model(model), m_diagnostics(diagnostics)
    {
    }

    void Read()
    {
        auto& deck = m_model.PowerPoint.emplace();
        Size slideIndex = 1;
        for (const auto& slide : m_editor.Slides())
        {
            if (!slide)
            {
                continue;
            }
            PptSlide modelSlide;
            if (auto layout = slide->Layout())
            {
                modelSlide.LayoutName = layout->Name();
            }
            modelSlide.Hidden = slide->IsHidden();
            modelSlide.NotesText = slide->NotesText();

            // Placeholder tagging: map slide-stored placeholder host elements to types.
            std::vector<std::pair<std::shared_ptr<OpenXMLElement>, std::string>> placeholders;
            for (const auto& placeholder : slide->Placeholders(false))
            {
                if (placeholder && placeholder->GetElement())
                {
                    placeholders.emplace_back(placeholder->GetElement(),
                                              PlaceholderToken(placeholder->Type()));
                }
            }

            const auto context = "slide " + std::to_string(slideIndex);
            if (auto tree = slide->ShapeTree())
            {
                Size shapeIndex = 1;
                for (const auto& shape : tree->Shapes())
                {
                    if (shape)
                    {
                        modelSlide.Shapes.push_back(ReadShape(
                            *shape, placeholders, context + " shape " + std::to_string(shapeIndex)));
                    }
                    ++shapeIndex;
                }
            }

            for (const auto& comment : slide->Comments())
            {
                PptCommentModel modelComment;
                modelComment.Author = comment.AuthorId;
                modelComment.Text = comment.Text;
                modelSlide.Comments.push_back(std::move(modelComment));
            }

            deck.Slides.push_back(std::move(modelSlide));
            ++slideIndex;
        }
    }

private:
    PptShape ReadShape(P::PresentationShape& shape,
                       const std::vector<std::pair<std::shared_ptr<OpenXMLElement>, std::string>>& placeholders,
                       const std::string& context)
    {
        PptShape model;
        model.Name = ShapeName(shape);
        if (const auto transform = shape.GetTransform())
        {
            model.Transform = TransformToModel(*transform);
        }

        if (shape.IsGroup())
        {
            model.Kind = PptShape::Type::Group;
            Size childIndex = 1;
            for (const auto& child : shape.Children())
            {
                if (child)
                {
                    model.Children.push_back(
                        ReadShape(*child, placeholders, context + " child " + std::to_string(childIndex)));
                }
                ++childIndex;
            }
            return model;
        }

        if (const auto table = shape.GetTable())
        {
            model.Kind = PptShape::Type::Table;
            model.Table = TableToModel(*table);
            return model;
        }

        if (const auto picture = shape.GetPicture())
        {
            model.Kind = PptShape::Type::Picture;
            model.AltText = picture->AltText.empty() ? picture->Title : picture->AltText;
            if (picture->Embedded)
            {
                MediaReference media;
                media.Id = "media" + std::to_string(m_model.Media.size() + 1);
                media.ContentType = picture->Embedded->ContentType;
                if (m_options.IncludeMediaData)
                {
                    media.Data = picture->Embedded->Data;
                }
                model.MediaId = media.Id;
                m_model.Media.push_back(std::move(media));
            }
            else
            {
                Warn(m_diagnostics, "Linked picture exported without media payload", context);
            }
            return model;
        }

        const auto frame = shape.GetTextFrame();

        std::string placeholderType;
        if (auto element = shape.GetElement())
        {
            for (const auto& [placeholderElement, token] : placeholders)
            {
                if (placeholderElement->IsSameNode(*element))
                {
                    placeholderType = token;
                    break;
                }
            }
        }
        // Title text boxes authored by SlideBuilder::SetTitle carry no p:ph
        // element; recognize them by their non-visual name.
        if (placeholderType.empty() && frame && model.Name == "Title")
        {
            placeholderType = "title";
        }

        if (!placeholderType.empty())
        {
            model.Kind = PptShape::Type::Placeholder;
            model.PlaceholderType = placeholderType;
            if (frame)
            {
                model.Text = FrameToModel(*frame);
            }
            return model;
        }

        if (frame)
        {
            model.Kind = PptShape::Type::TextBox;
            model.Text = FrameToModel(*frame);
            return model;
        }

        model.Kind = PptShape::Type::Other;
        Warn(m_diagnostics, "Unsupported shape exported without content (chart/media/OLE)", context);
        return model;
    }

    P::PowerPointDocumentEditor& m_editor;
    ModelReadOptions m_options;
    DocumentModel& m_model;
    std::vector<ToolDiagnostic>& m_diagnostics;
};

// --- Writer -----------------------------------------------------------------

class PowerPointModelWriter
{
public:
    PowerPointModelWriter(const DocumentModel& model, std::vector<ToolDiagnostic>& diagnostics)
        : m_model(model), m_diagnostics(diagnostics)
    {
    }

    bool Write(const std::filesystem::path& path)
    {
        auto editor = P::PowerPointDocumentEditor::CreateNew();
        if (!editor || !m_model.PowerPoint)
        {
            m_diagnostics.push_back(
                ToolDiagnostic{ToolSeverity::Error, "Cannot create PowerPoint document from model"});
            return false;
        }

        Size slideIndex = 1;
        for (const auto& modelSlide : m_model.PowerPoint->Slides)
        {
            WriteSlide(*editor, modelSlide, "slide " + std::to_string(slideIndex));
            ++slideIndex;
        }

        if (auto document = editor->GetDocument())
        {
            const auto writeProperty = [&](std::string_view name, const std::string& value)
            {
                if (!value.empty())
                {
                    WriteCoreProperty(*document, name, value);
                }
            };
            writeProperty("Title", m_model.Properties.Title);
            writeProperty("Subject", m_model.Properties.Subject);
            writeProperty("Creator", m_model.Properties.Creator);
            writeProperty("Keywords", m_model.Properties.Keywords);
            writeProperty("Description", m_model.Properties.Description);
            writeProperty("Category", m_model.Properties.Category);
        }

        if (!editor->SaveToFile(path))
        {
            m_diagnostics.push_back(
                ToolDiagnostic{ToolSeverity::Error, "Failed to save PowerPoint document", path.string()});
            return false;
        }
        return true;
    }

private:
    void WriteSlide(P::PowerPointDocumentEditor& editor, const PptSlide& modelSlide,
                    const std::string& context)
    {
        // The title placeholder is authored through the slide builder; the
        // remaining shapes are appended to the shape tree afterwards.
        auto builder = editor.CreateSlideBuilder();
        builder.SetHidden(modelSlide.Hidden);

        const PptShape* titleShape = nullptr;
        std::vector<const PptShape*> contentShapes;
        CollectShapes(modelSlide.Shapes, titleShape, contentShapes);

        auto slide = editor.AddSlide(builder);
        if (!slide)
        {
            Warn(m_diagnostics, "Slide could not be created", context);
            return;
        }

        auto tree = slide->ShapeTree();
        if (!tree)
        {
            Warn(m_diagnostics, "Slide has no shape tree", context);
            return;
        }

        if (titleShape != nullptr && titleShape->Text)
        {
            WriteTitle(*slide, *tree, *titleShape, context);
        }

        // Stack shapes without an explicit transform below the title area.
        Size defaultsNeeded = 0;
        for (const auto* shape : contentShapes)
        {
            if (!shape->Transform.Present)
            {
                ++defaultsNeeded;
            }
        }
        const Int64 contentTop = 1600200;
        const Int64 contentHeight = 4525963;
        const Int64 slice =
            defaultsNeeded > 0 ? contentHeight / static_cast<Int64>(defaultsNeeded) : contentHeight;
        Int64 nextTop = contentTop;

        for (const auto* shape : contentShapes)
        {
            P::PresentationShapeTransform transform;
            if (shape->Transform.Present)
            {
                transform = TransformFromModel(shape->Transform);
            }
            else
            {
                transform = DefaultTransform(457200, nextTop, 8229600, slice);
                nextTop += slice;
            }
            WriteShape(*tree, *shape, transform, context);
        }

        if (!modelSlide.NotesText.empty())
        {
            slide->SetNotesText(modelSlide.NotesText);
        }
        if (!modelSlide.Comments.empty())
        {
            Warn(m_diagnostics, "Slide comments are not written back", context);
        }
        if (!modelSlide.LayoutName.empty())
        {
            // Layout assignment by name is not attempted: the new package's
            // layout catalog differs from the source presentation's.
        }
    }

    void WriteTitle(P::PresentationSlide& slide, P::PresentationShapeTree& tree, const PptShape& modelShape,
                    const std::string& context)
    {
        // Author a real title placeholder so consumers (and our own reader)
        // recognize the shape's role.
        P::PresentationShape::Ptr target;
        auto placeholder = slide.AddPlaceholder(PresentationPlaceholderValues::Title);
        if (placeholder && placeholder->GetElement())
        {
            for (const auto& shape : tree.Shapes())
            {
                if (shape && shape->GetElement() &&
                    shape->GetElement()->IsSameNode(*placeholder->GetElement()))
                {
                    target = shape;
                    break;
                }
            }
        }
        if (!target)
        {
            target = tree.AddShape("Title");
        }
        if (!target)
        {
            Warn(m_diagnostics, "Title shape could not be created", context);
            return;
        }
        const auto transform = modelShape.Transform.Present
                                   ? TransformFromModel(modelShape.Transform)
                                   : DefaultTransform(457200, 274638, 8229600, 1143000);
        target->SetTransform(transform);
        target->SetTextFrame(FrameFromModel(*modelShape.Text));
    }

    void CollectShapes(const std::vector<PptShape>& shapes, const PptShape*& titleShape,
                       std::vector<const PptShape*>& contentShapes)
    {
        for (const auto& shape : shapes)
        {
            if (titleShape == nullptr && shape.Kind == PptShape::Type::Placeholder &&
                IsTitleToken(shape.PlaceholderType) && shape.Text)
            {
                titleShape = &shape;
                continue;
            }
            contentShapes.push_back(&shape);
        }
    }

    void WriteShape(P::PresentationShapeTree& tree, const PptShape& modelShape,
                    const P::PresentationShapeTransform& transform, const std::string& context)
    {
        switch (modelShape.Kind)
        {
            case PptShape::Type::Group:
            {
                // Group authoring is flattened: children become top-level shapes.
                Warn(m_diagnostics, "Group flattened to individual shapes", context);
                for (const auto& child : modelShape.Children)
                {
                    const auto childTransform = child.Transform.Present
                                                    ? TransformFromModel(child.Transform)
                                                    : transform;
                    WriteShape(tree, child, childTransform, context);
                }
                break;
            }
            case PptShape::Type::Table:
            {
                if (modelShape.Table)
                {
                    if (!tree.AddTable(TableFromModel(*modelShape.Table, transform)))
                    {
                        Warn(m_diagnostics, "Table could not be created", context);
                    }
                }
                break;
            }
            case PptShape::Type::Picture:
            {
                const MediaReference* media = nullptr;
                for (const auto& candidate : m_model.Media)
                {
                    if (candidate.Id == modelShape.MediaId)
                    {
                        media = &candidate;
                        break;
                    }
                }
                if (media == nullptr || media->Data.empty())
                {
                    Warn(m_diagnostics, "Picture media payload missing; picture skipped",
                         modelShape.MediaId);
                    break;
                }
                const auto pictureTransform =
                    modelShape.Transform.Present ? transform : P::PresentationShapeTransform{};
                auto picture = tree.AddPictureFromData(media->Data, pictureTransform, modelShape.Name);
                if (!picture)
                {
                    // Fall back to the explicit payload path for formats the
                    // signature-based helper does not recognize.
                    P::PresentationPictureData data;
                    data.Embedded = P::PresentationEmbeddedPicture{media->Data, media->ContentType};
                    data.AltText = modelShape.AltText;
                    data.Transform = transform;
                    picture = tree.AddPicture(data);
                }
                if (!picture)
                {
                    Warn(m_diagnostics, "Picture could not be created", context);
                }
                break;
            }
            case PptShape::Type::TextBox:
            case PptShape::Type::Placeholder:
            {
                if (!modelShape.Text)
                {
                    break;
                }
                auto shape = tree.AddShape(modelShape.Name);
                if (!shape)
                {
                    Warn(m_diagnostics, "Text shape could not be created", context);
                    break;
                }
                shape->SetTransform(transform);
                shape->SetTextFrame(FrameFromModel(*modelShape.Text));
                break;
            }
            case PptShape::Type::Other:
            {
                Warn(m_diagnostics, "Unsupported shape dropped", context);
                break;
            }
        }
    }

    const DocumentModel& m_model;
    std::vector<ToolDiagnostic>& m_diagnostics;
};

} // namespace

DocumentModel ReadPowerPointModel(const std::filesystem::path& path, const ModelReadOptions& options,
                                  std::vector<ToolDiagnostic>& diagnostics)
{
    auto editor = P::PowerPointDocumentEditor::Open(path, UntrustedOpenSettings());
    if (!editor)
    {
        diagnostics.push_back(
            ToolDiagnostic{ToolSeverity::Error, "Failed to open PowerPoint document", path.string()});
        return {};
    }

    return ReadPowerPointModel(*editor, options, diagnostics);
}

DocumentModel ReadPowerPointModel(P::PowerPointDocumentEditor& editor, const ModelReadOptions& options,
                                  std::vector<ToolDiagnostic>& diagnostics)
{
    DocumentModel model;
    model.Family = DocumentFamily::PowerPoint;
    if (auto document = editor.GetDocument())
    {
        model.Properties = ReadCoreProperties(*document);
    }

    PowerPointModelReader reader(editor, options, model, diagnostics);
    reader.Read();
    return model;
}

bool WritePowerPointModel(const DocumentModel& model, const std::filesystem::path& path,
                          std::vector<ToolDiagnostic>& diagnostics)
{
    if (!model.PowerPoint)
    {
        diagnostics.push_back(ToolDiagnostic{ToolSeverity::Error, "Model carries no PowerPoint presentation"});
        return false;
    }
    PowerPointModelWriter writer(model, diagnostics);
    return writer.Write(path);
}

} // namespace ExyokiOffice::Tools
