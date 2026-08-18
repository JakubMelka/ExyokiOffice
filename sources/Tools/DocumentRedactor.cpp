// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Tools/DocumentRedactor.hpp"
#include "ExyokiOffice/Tools/PackageLimits.hpp"

#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"
#include "ExyokiOffice/Tools/PackageInspector.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Wordprocessing.hpp"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ExyokiOffice::Tools
{

namespace W = ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing;

/// The WordprocessingML namespace, for the attributes that have no typed
/// accessor of their own.
constexpr std::string_view kWordNamespace = "http://schemas.openxmlformats.org/wordprocessingml/2006/main";

/// File-local helpers behind the redaction passes.
class DocumentRedactorHelper
{
public:
    static void AddError(std::vector<ToolDiagnostic>& diagnostics, std::string message, std::string context = {})
    {
        diagnostics.push_back(ToolDiagnostic{ToolSeverity::Error, std::move(message), std::move(context)});
    }

    static void AddInfo(std::vector<ToolDiagnostic>& diagnostics, std::string message, std::string context = {})
    {
        diagnostics.push_back(ToolDiagnostic{ToolSeverity::Info, std::move(message), std::move(context)});
    }

    static void AddWarning(std::vector<ToolDiagnostic>& diagnostics, std::string message, std::string context = {})
    {
        diagnostics.push_back(ToolDiagnostic{ToolSeverity::Warning, std::move(message), std::move(context)});
    }

    /**
     * @brief Repeats what the loader said about content it could not read.
     *
     * A redaction reports on the document it was given, and these two say that
     * the document is not all of the file: a part the archive would not give up,
     * or relationships that were not XML. Redacting the rest then succeeds
     * without having been able to look at what is missing, and a caller reading
     * only `RedactResult` would take "Ok" for "nothing carrying that content is
     * left". Everything else the validator has to say is a statement about the
     * markup and belongs to validation, not here.
     */
    static void CarryLoadDiagnostics(RedactResult& result, const OpenXmlPackage* package)
    {
        if (package == nullptr)
        {
            return;
        }

        for (const auto& issue : package->LastValidationResult().Issues())
        {
            if (issue.Id != ValidationErrorId::OpcEntryUnreadable &&
                issue.Id != ValidationErrorId::OpcMalformedPartXml)
            {
                continue;
            }
            AddWarning(result.Diagnostics, "Part of the document could not be read and was not redacted",
                       issue.Message);
        }
    }

    /// Word content types whose XML carries comment markers, revisions and
    /// hidden runs.
    ///
    /// The comment part is one of them. It is detached whole when comments are
    /// being removed, but a caller who keeps the comments still asked for the
    /// revisions inside them to be accepted, and a tracked deletion in a comment
    /// carries an author and a date exactly like one in the body.
    [[nodiscard]] static bool IsWordStoryContentType(const std::string& contentType)
    {
        if (contentType.find("wordprocessingml") == std::string::npos &&
            contentType.find("ms-word") == std::string::npos)
        {
            return false;
        }
        return contentType.ends_with("main+xml") || contentType.ends_with("header+xml") ||
               contentType.ends_with("footer+xml") || contentType.ends_with("footnotes+xml") ||
               contentType.ends_with("endnotes+xml") || contentType.ends_with("glossary+xml") ||
               contentType.ends_with(".comments+xml");
    }

    /// Word parts that exist solely to store comments (and their author registry).
    [[nodiscard]] static bool IsWordCommentContentType(const std::string& contentType)
    {
        if (contentType.find("wordprocessingml") == std::string::npos &&
            contentType.find("ms-word") == std::string::npos)
        {
            return false;
        }
        return contentType.ends_with(".comments+xml") || contentType.ends_with("commentsExtended+xml") ||
               contentType.ends_with("commentsIds+xml") || contentType.ends_with("commentsExtensible+xml") ||
               contentType.ends_with("people+xml");
    }

    /// The Word style definitions, which is where hidden text can be declared
    /// without any run saying so.
    [[nodiscard]] static bool IsWordStylesContentType(const std::string& contentType)
    {
        return contentType.find("wordprocessingml") != std::string::npos && contentType.ends_with("styles+xml");
    }

    /// The Word settings part, which is where the `w:rsids` registry lists every
    /// revision-save id the document has ever carried.
    [[nodiscard]] static bool IsWordSettingsContentType(const std::string& contentType)
    {
        return contentType.find("wordprocessingml") != std::string::npos && contentType.ends_with("settings+xml");
    }

    /// True for a part that belongs to the glossary (building-block) document
    /// rather than to the main document.
    ///
    /// The glossary lives under `word/glossary/` and can carry its own styles
    /// part, so a `styleId` defined there is a different style from the same id
    /// in the main document. Which graph a part belongs to decides which styles
    /// part governs it.
    [[nodiscard]] static bool IsGlossaryUri(const std::string& uri)
    {
        return uri.find("/glossary/") != std::string::npos;
    }

    /**
     * @brief Parts that carry identity or content the document does not show.
     *
     * The thumbnail is the reason this list exists: it is a rendering of the
     * first page, produced before anything was redacted, and it survives every
     * edit made to the document body. `customXml` is next to it because a data
     * store bound to content controls keeps the values that were bound, whatever
     * the visible text says now.
     */
    [[nodiscard]] static bool IsPrivacySensitivePart(const PartRecord& record)
    {
        // By URI rather than by content type: the thumbnail is whatever image
        // format the producing application chose, and Word writes it with no
        // content type of its own at all.
        return record.Uri.starts_with("/docProps/thumbnail") || record.Uri.starts_with("/customXml/");
    }

    /**
     * @brief The registries that name the authors of comments.
     *
     * Threaded comments in a workbook are attributed through the person
     * registry, and PowerPoint keeps its comment authors in one of its own. Both
     * name people, so both go - but only together with the comments that point
     * at them. Removing the registry from a document whose comments were kept
     * leaves every `personId` and every author reference dangling, which is a
     * broken document rather than a redacted one.
     */
    [[nodiscard]] static bool IsCommentAuthorRegistryPart(const PartRecord& record)
    {
        return record.ContentType.ends_with("person+xml") || record.ContentType.ends_with("commentAuthors+xml");
    }

    /// Legacy (pre-2021) PowerPoint comment parts, which the typed API does not reach.
    [[nodiscard]] static bool IsLegacyPowerPointCommentPart(const PartRecord& record)
    {
        return record.ContentType.find("presentationml") != std::string::npos &&
               record.ContentType.ends_with("comments+xml") && record.Uri.starts_with("/ppt/comments/");
    }

    /// Every element of the subtree, without recursing once per level.
    ///
    /// Redaction runs over parts a caller did not produce, and an editor handed
    /// in by the caller may have been opened with no depth limit at all, so the
    /// walk is bounded by memory rather than by the process stack. Children are
    /// taken as a snapshot, because a visitor is allowed to change the tree.
    template <typename TVisit>
    static void ForEachElement(const std::shared_ptr<OpenXMLElement>& root, TVisit&& visit)
    {
        if (!root)
        {
            return;
        }

        std::vector<std::shared_ptr<OpenXMLElement>> pending{root};
        while (!pending.empty())
        {
            auto element = pending.back();
            pending.pop_back();
            visit(element);
            for (const auto& child : element->ChildrenInContentModel())
            {
                if (child)
                {
                    pending.push_back(child);
                }
            }
        }
    }

    /// True for the elements that record what a property looked like before a
    /// tracked change: accepting the change means keeping the current value and
    /// dropping the record of the old one.
    ///
    /// Asked of the typed model rather than of the element's name. `w:ins`
    /// alone stands for three different types depending on where it sits, and
    /// the same local name occurs in vocabularies this pass must not touch; the
    /// cast answers for the element the parser actually resolved.
    [[nodiscard]] static bool IsPropertyChangeRecord(const std::shared_ptr<OpenXMLElement>& element)
    {
        return openxmlelement_cast<W::ParagraphPropertiesChange>(element) ||
               openxmlelement_cast<W::RunPropertiesChange>(element) ||
               openxmlelement_cast<W::ParagraphMarkRunPropertiesChange>(element) ||
               openxmlelement_cast<W::SectionPropertiesChange>(element) ||
               openxmlelement_cast<W::TablePropertiesChange>(element) ||
               openxmlelement_cast<W::TableRowPropertiesChange>(element) ||
               openxmlelement_cast<W::TableCellPropertiesChange>(element) ||
               openxmlelement_cast<W::TablePropertyExceptionsChange>(element) ||
               openxmlelement_cast<W::TableGridChange>(element) ||
               openxmlelement_cast<W::NumberingChange>(element) ||
               openxmlelement_cast<W::CellInsertion>(element) || openxmlelement_cast<W::CellDeletion>(element) ||
               openxmlelement_cast<W::CellMerge>(element);
    }

    /**
     * @brief True for the property-level markers that record a tracked change.
     *
     * These are `CT_TrackChange` elements, not wrappers: `w:trPr/w:ins` says the
     * row was inserted, `w:pPr/w:rPr/w:del` says the paragraph mark was deleted.
     * They hold no content, so accepting the change is a matter of applying
     * whatever the marker implies structurally and then dropping the marker -
     * and every one of them carries `w:author` and `w:date`, which is why
     * leaving them behind publishes the reviewer's name from a document whose
     * revisions are reported as accepted.
     */
    [[nodiscard]] static bool IsPropertyRevisionRecord(const std::shared_ptr<OpenXMLElement>& element)
    {
        return openxmlelement_cast<W::Inserted>(element) || openxmlelement_cast<W::Deleted>(element) ||
               openxmlelement_cast<W::MoveFrom>(element) || openxmlelement_cast<W::MoveTo>(element);
    }

    /// True for a wrapper whose content stays when the change is accepted.
    [[nodiscard]] static bool IsAcceptedInsertion(const std::shared_ptr<OpenXMLElement>& element)
    {
        return openxmlelement_cast<W::InsertedRun>(element) || openxmlelement_cast<W::MoveToRun>(element);
    }

    /// True for a wrapper whose content goes when the change is accepted.
    [[nodiscard]] static bool IsAcceptedDeletion(const std::shared_ptr<OpenXMLElement>& element)
    {
        return openxmlelement_cast<W::DeletedRun>(element) || openxmlelement_cast<W::MoveFromRun>(element);
    }

    /// Range markers a move leaves behind; they carry the author but no content.
    [[nodiscard]] static bool IsMoveRangeMarker(const std::shared_ptr<OpenXMLElement>& element)
    {
        return openxmlelement_cast<W::MoveFromRangeStart>(element) ||
               openxmlelement_cast<W::MoveFromRangeEnd>(element) ||
               openxmlelement_cast<W::MoveToRangeStart>(element) || openxmlelement_cast<W::MoveToRangeEnd>(element);
    }

    /// True for the in-text markers that point at a comment.
    [[nodiscard]] static bool IsCommentMarker(const std::shared_ptr<OpenXMLElement>& element)
    {
        return openxmlelement_cast<W::CommentRangeStart>(element) ||
               openxmlelement_cast<W::CommentRangeEnd>(element) || openxmlelement_cast<W::CommentReference>(element);
    }

    /// The first child of @p element of the given type, or nullptr.
    template <typename TChild>
    [[nodiscard]] static std::shared_ptr<TChild> ChildOfType(const std::shared_ptr<OpenXMLElement>& element)
    {
        if (!element)
        {
            return nullptr;
        }
        for (const auto& child : element->ChildrenInContentModel())
        {
            if (auto typed = openxmlelement_cast<TChild>(child))
            {
                return typed;
            }
        }
        return nullptr;
    }

    /**
     * @brief Merges a paragraph whose paragraph mark was deleted into the next one.
     *
     * `w:pPr/w:rPr/w:del` says the paragraph mark itself is a tracked deletion.
     * Accepting that means the two paragraphs become one, which is why removing
     * the marker on its own is not enough: it would leave the split in place and
     * quietly turn an accept into a reject. `w:moveFrom` on the same mark says
     * it went with a moved region, which comes to the same thing here.
     */
    static Size MergeDeletedParagraphMarks(const std::shared_ptr<OpenXMLElement>& root)
    {
        Size merged = 0;
        ForEachElement(root,
                       [&merged](const std::shared_ptr<OpenXMLElement>& parent)
                       {
                           const auto children = parent->ChildrenInContentModel();
                           for (Size index = 0; index < children.size(); ++index)
                           {
                               auto paragraph = openxmlelement_cast<W::Paragraph>(children[index]);
                               if (!paragraph)
                               {
                                   continue;
                               }

                               const auto properties = ChildOfType<W::ParagraphProperties>(paragraph);
                               const auto markProperties = ChildOfType<W::ParagraphMarkRunProperties>(properties);
                               std::shared_ptr<OpenXMLElement> deleted = ChildOfType<W::Deleted>(markProperties);
                               if (!deleted)
                               {
                                   deleted = ChildOfType<W::MoveFrom>(markProperties);
                               }
                               if (!deleted)
                               {
                                   continue;
                               }

                               const auto next = index + 1 < children.size()
                                                     ? openxmlelement_cast<W::Paragraph>(children[index + 1])
                                                     : nullptr;
                               if (!next)
                               {
                                   // Nothing to merge into - the last paragraph
                                   // of a cell or of the body. Dropping the
                                   // marker is all that is left to do.
                                   deleted->Remove();
                                   continue;
                               }

                               // The content moves to the front of the following
                               // paragraph, after that paragraph's own
                               // properties, so the merged text keeps its order.
                               std::shared_ptr<OpenXMLElement> anchor = ChildOfType<W::ParagraphProperties>(next);
                               for (const auto& content : paragraph->ChildrenInContentModel())
                               {
                                   if (!content || content == properties)
                                   {
                                       continue;
                                   }
                                   auto moved = anchor ? content->MoveAfter(anchor)
                                                       : content->MoveInto(next, next->ChildrenInContentModel().empty()
                                                                                     ? nullptr
                                                                                     : next->ChildrenInContentModel().front());
                                   if (moved)
                                   {
                                       anchor = moved;
                                   }
                               }
                               paragraph->Remove();
                               ++merged;
                           }
                       });
        return merged;
    }

    /// Deletes table rows whose `w:trPr/w:del` marks the row itself as removed.
    static Size RemoveDeletedTableRows(const std::shared_ptr<OpenXMLElement>& root)
    {
        Size removed = 0;
        ForEachElement(root,
                       [&removed](const std::shared_ptr<OpenXMLElement>& parent)
                       {
                           for (const auto& child : parent->ChildrenInContentModel())
                           {
                               auto row = openxmlelement_cast<W::TableRow>(child);
                               if (!row)
                               {
                                   continue;
                               }
                               const auto properties = ChildOfType<W::TableRowProperties>(row);
                               if (ChildOfType<W::Deleted>(properties))
                               {
                                   row->Remove();
                                   ++removed;
                               }
                           }
                       });
        return removed;
    }

    /**
     * @brief Accepts every remaining tracked change in one part.
     *
     * Insertions and move destinations are unwrapped so their content stays,
     * deletions and move sources go with their content, and the records of
     * former property values are dropped. Run structurally first
     * (MergeDeletedParagraphMarks, RemoveDeletedTableRows): those two need the
     * `w:del` markers this pass removes.
     */
    static void AcceptRevisions(const std::shared_ptr<OpenXMLElement>& root, Size& accepted)
    {
        if (!root)
        {
            return;
        }

        std::vector<std::shared_ptr<OpenXMLElement>> pending{root};
        while (!pending.empty())
        {
            auto element = pending.back();
            pending.pop_back();

            for (const auto& child : element->ChildrenInContentModel())
            {
                if (!child)
                {
                    continue;
                }

                if (IsAcceptedInsertion(child))
                {
                    // The content stays and the wrapper goes. The promoted
                    // children are examined in turn, because a tracked change
                    // can be nested inside another one.
                    for (const auto& promoted : child->ReplaceWithChildren())
                    {
                        if (promoted)
                        {
                            pending.push_back(promoted);
                        }
                    }
                    ++accepted;
                    continue;
                }

                if (IsAcceptedDeletion(child))
                {
                    child->Remove();
                    ++accepted;
                    continue;
                }

                if (IsPropertyChangeRecord(child) || IsPropertyRevisionRecord(child))
                {
                    // The structural passes above have already applied whatever
                    // a property-level marker meant for the document; what is
                    // left here is the record of who made the change and when.
                    child->Remove();
                    ++accepted;
                    continue;
                }

                if (IsMoveRangeMarker(child))
                {
                    // A marker, not a change: removed so that no author name is
                    // left behind, but not counted as a change of its own.
                    child->Remove();
                    continue;
                }

                pending.push_back(child);
            }
        }
    }

    /// Removes the `w:rsid*` attributes, which tie edits to an editing session.
    ///
    /// The names are the fixed set the schema defines, written as qualified
    /// names rather than matched as a prefix, so this cannot reach an attribute
    /// of another vocabulary that happens to start the same way.
    static Size RemoveRevisionSaveIds(const std::shared_ptr<OpenXMLElement>& root)
    {
        static const std::array<OpenXmlQualifiedName, 8> kRevisionSaveIds{
            OpenXmlQualifiedName(kWordNamespace, "rsid"),
            OpenXmlQualifiedName(kWordNamespace, "rsidDel"),
            OpenXmlQualifiedName(kWordNamespace, "rsidP"),
            OpenXmlQualifiedName(kWordNamespace, "rsidR"),
            OpenXmlQualifiedName(kWordNamespace, "rsidRDefault"),
            OpenXmlQualifiedName(kWordNamespace, "rsidRPr"),
            OpenXmlQualifiedName(kWordNamespace, "rsidSect"),
            OpenXmlQualifiedName(kWordNamespace, "rsidTr")};

        Size removed = 0;
        ForEachElement(root,
                       [&removed](const std::shared_ptr<OpenXMLElement>& element)
                       {
                           for (const auto& name : kRevisionSaveIds)
                           {
                               if (element->HasAttribute(name))
                               {
                                   element->RemoveAttribute(name);
                                   ++removed;
                               }
                           }
                       });
        return removed;
    }

    /// Scrubs the revision-save ids from the settings part.
    ///
    /// The per-story pass reaches the `w:rsid*` attributes scattered through the
    /// body, but `word/settings.xml` is not a story part, and it holds the
    /// `w:rsids` registry: a list of every editing session that ever touched the
    /// file. Left behind, it ties a redacted document back to the machines and
    /// sittings that produced it, so the registry element goes whole, and any
    /// stray attributes on the settings tree go with it.
    static Size ScrubWordSettings(const std::shared_ptr<OpenXMLElement>& root)
    {
        if (!root)
        {
            return 0;
        }
        Size removed = RemoveRevisionSaveIds(root);

        // `ChildOfType` would stop at the first registry, but the loader does not
        // run strict schema validation, so a non-conformant settings part can
        // carry more than one `w:rsids`, and leaving the second behind would
        // republish exactly the editing-session ids this pass exists to remove.
        // Collect every registry in the subtree first - removing during the walk
        // would mutate the tree it reads - then drop them all, so the scrub is
        // fail-closed even for malformed input.
        std::vector<std::shared_ptr<W::Rsids>> registries;
        ForEachElement(root,
                       [&registries](const std::shared_ptr<OpenXMLElement>& element)
                       {
                           if (auto rsids = openxmlelement_cast<W::Rsids>(element))
                           {
                               registries.push_back(std::move(rsids));
                           }
                       });
        for (const auto& rsids : registries)
        {
            rsids->Remove();
            ++removed;
        }
        return removed;
    }

    /// True when a `w:vanish` or `w:specVanish` actually hides its run.
    ///
    /// Both are `CT_OnOff`, so both can be switched off with `w:val="false"` -
    /// which is how a style that hides text is undone for one run, and how a run
    /// that carries the element is nevertheless visible. Taking the element's
    /// presence for the answer would delete text the document displays.
    [[nodiscard]] static bool HidesText(const std::shared_ptr<W::OnOffType>& onOff)
    {
        // A missing value means "on"; only an explicit false turns it off.
        return onOff && onOff->GetVal().ValueOr(true);
    }

    /**
     * @brief Style identifiers whose runs are hidden text.
     *
     * `w:vanish` in a style hides every run that uses it without the run saying
     * anything, so a redactor that only reads direct run properties publishes
     * exactly the text the author hid. Inheritance is followed through
     * `w:basedOn`, with a visit set because a corrupted style graph can be
     * circular.
     */
    [[nodiscard]] static std::unordered_set<std::string> HiddenStyleIds(const std::shared_ptr<W::Styles>& styles)
    {
        struct StyleEntry
        {
            std::string BasedOn;
            std::optional<bool> Vanish;
        };

        std::unordered_map<std::string, StyleEntry> definitions;
        std::unordered_set<std::string> hidden;
        if (!styles)
        {
            return hidden;
        }

        for (const auto& style : styles->Elements<W::Style>())
        {
            if (!style || !style->GetStyleId().IsDefined())
            {
                continue;
            }

            StyleEntry entry;
            if (const auto basedOn = ChildOfType<W::BasedOn>(style); basedOn && basedOn->GetVal().IsDefined())
            {
                entry.BasedOn = basedOn->GetVal().ToString();
            }
            // `w:rPr` inside a style is StyleRunProperties, a different type
            // from the run properties of a run - which is exactly the kind of
            // distinction a name-based test cannot make.
            if (const auto runProperties = ChildOfType<W::StyleRunProperties>(style))
            {
                if (const auto vanish = ChildOfType<W::Vanish>(runProperties))
                {
                    entry.Vanish = HidesText(vanish);
                }
                else if (const auto specVanish = ChildOfType<W::SpecVanish>(runProperties))
                {
                    entry.Vanish = HidesText(specVanish);
                }
            }
            definitions.emplace(style->GetStyleId().ToString(), std::move(entry));
        }

        for (const auto& [id, entry] : definitions)
        {
            std::unordered_set<std::string> visited;
            const StyleEntry* current = &entry;
            std::string cursor = id;
            while (current)
            {
                if (current->Vanish)
                {
                    if (*current->Vanish)
                    {
                        hidden.insert(id);
                    }
                    break;
                }
                if (!visited.insert(cursor).second || current->BasedOn.empty())
                {
                    break;
                }
                const auto parent = definitions.find(current->BasedOn);
                if (parent == definitions.end())
                {
                    break;
                }
                cursor = current->BasedOn;
                current = &parent->second;
            }
        }

        return hidden;
    }

    /// Detaches every incoming relationship edge of the part, which removes the
    /// part from the package once it is unreachable.
    static bool DetachPart(OpenXmlPackage& package, const PartRecord& record,
                           std::vector<ToolDiagnostic>& diagnostics)
    {
        auto part = package.GetPartByUri(record.Uri);
        if (!part)
        {
            return false;
        }
        for (const auto& edge : record.Incoming)
        {
            OpenXmlPartContainer* source = nullptr;
            if (edge.SourceUri == "/")
            {
                source = &package;
            }
            else
            {
                source = package.GetPartByUri(edge.SourceUri).get();
            }
            if (!source)
            {
                AddError(diagnostics, "Relationship source part not found", edge.SourceUri);
                return false;
            }
            while (source->RemovePartReference(part))
            {
            }
        }
        return true;
    }

    /**
     * @brief True when the run is hidden text.
     *
     * Direct `w:vanish` decides on its own. Failing that the run's style
     * decides, because a style can hide text with nothing in the run to show for
     * it - `w:vanish` inside `w:style/w:rPr` is how "hidden" is applied to a
     * whole class of runs, and it is exactly what an author uses to keep notes
     * in a document they intend to publish. Either element switched off with
     * `w:val="false"` says the run is visible, and says it about this run rather
     * than about its style, so the style is not consulted afterwards.
     */
    [[nodiscard]] static bool IsHiddenRun(const std::shared_ptr<W::Run>& run,
                                          const std::unordered_set<std::string>& hiddenStyles)
    {
        const auto properties = ChildOfType<W::RunProperties>(run);
        if (!properties)
        {
            return false;
        }

        if (const auto vanish = ChildOfType<W::Vanish>(properties))
        {
            return HidesText(vanish);
        }
        if (const auto specVanish = ChildOfType<W::SpecVanish>(properties))
        {
            return HidesText(specVanish);
        }

        const auto style = ChildOfType<W::RunStyle>(properties);
        return style && style->GetVal().IsDefined() && hiddenStyles.contains(style->GetVal().ToString());
    }

    /// Strips comment markers and/or hidden runs from one part.
    static void ScrubWordPart(const std::shared_ptr<OpenXMLElement>& root, bool removeCommentMarkers,
                              bool removeHiddenRuns, const std::unordered_set<std::string>& hiddenStyles,
                              Size& commentMarkersRemoved, Size& hiddenRunsRemoved)
    {
        if (!root)
        {
            return;
        }

        std::vector<std::shared_ptr<OpenXMLElement>> pending{root};
        while (!pending.empty())
        {
            auto element = pending.back();
            pending.pop_back();

            for (const auto& child : element->ChildrenInContentModel())
            {
                if (!child)
                {
                    continue;
                }
                if (removeCommentMarkers && IsCommentMarker(child))
                {
                    child->Remove();
                    ++commentMarkersRemoved;
                    continue;
                }
                if (removeHiddenRuns)
                {
                    if (auto run = openxmlelement_cast<W::Run>(child); run && IsHiddenRun(run, hiddenStyles))
                    {
                        run->Remove();
                        ++hiddenRunsRemoved;
                        continue;
                    }
                }
                pending.push_back(child);
            }
        }
    }

    /**
     * @brief Clears identity and descriptive metadata, and drops the parts that hold more of it.
     *
     * Names are the obvious case, but a document that has been through review
     * carries a good deal more: the title and keywords typed for an earlier
     * draft, the manager and company of whoever set up the template, the
     * revision count and the last print date, the name of the template itself,
     * and a thumbnail rendered from the first page before any of this was
     * removed. Publishing a document because its visible text was checked, and
     * shipping all of that with it, is the failure this is here to prevent.
     *
     * @param removeCommentAuthors Whether the comment author registries go too.
     *        They may only be removed together with the comments that refer to
     *        them, so this follows RedactOptions::RemoveComments rather than the
     *        option that brings us here.
     */
    static void RedactMetadata(OpenXmlPackage& package, RedactResult& result, bool removeCommentAuthors)
    {
        // Read first, and clear only what held something: removing a property
        // that was never there is reported as success either way, and counting
        // it would overstate what the redaction actually removed.
        const auto existing = ReadCoreProperties(package);
        const std::pair<const char*, const std::string*> identity[]{
            {"Creator", &existing.Creator},
            {"LastModifiedBy", &existing.LastModifiedBy},
            {"Company", &existing.Company},
            {"Title", &existing.Title},
            {"Subject", &existing.Subject},
            {"Description", &existing.Description},
            {"Keywords", &existing.Keywords},
            {"Category", &existing.Category},
            {"ContentStatus", &existing.ContentStatus}};

        for (const auto& [name, value] : identity)
        {
            if (!value->empty() && WriteCoreProperty(package, name, ""))
            {
                ++result.MetadataFieldsCleared;
            }
        }

        // These have no field in CoreProperties, so they are cleared blind: the
        // write reports whether there was anything to clear.
        for (const char* name : {"Manager", "HyperlinkBase", "Revision"})
        {
            if (WriteCoreProperty(package, name, ""))
            {
                ++result.MetadataFieldsCleared;
            }
        }

        Packaging::DocumentProperties properties(package);
        if (properties.GetLastPrinted() && properties.SetLastPrinted(std::nullopt))
        {
            ++result.MetadataFieldsCleared;
        }
        if (!properties.GetTemplate().empty() && properties.SetTemplate(""))
        {
            ++result.MetadataFieldsCleared;
        }

        for (const auto& record : ListParts(package))
        {
            if (record.ContentType == "application/vnd.openxmlformats-officedocument.custom-properties+xml")
            {
                if (DetachPart(package, record, result.Diagnostics))
                {
                    AddInfo(result.Diagnostics, "Custom properties part removed", record.Uri);
                    ++result.MetadataFieldsCleared;
                }
                continue;
            }

            if (IsPrivacySensitivePart(record) && DetachPart(package, record, result.Diagnostics))
            {
                AddInfo(result.Diagnostics, "Part removed because it carries data the document does not show",
                        record.Uri);
                ++result.PartsRemoved;
                continue;
            }

            if (IsCommentAuthorRegistryPart(record))
            {
                if (!removeCommentAuthors)
                {
                    AddInfo(result.Diagnostics,
                            "Comment author registry kept, because the comments referring to it were kept; "
                            "it still names the people who wrote them",
                            record.Uri);
                    continue;
                }
                if (DetachPart(package, record, result.Diagnostics))
                {
                    AddInfo(result.Diagnostics, "Comment author registry removed", record.Uri);
                    ++result.PartsRemoved;
                }
            }
        }
    }

    static RedactResult RedactWord(Word::WordDocumentEditor& editor, const RedactOptions& options)
    {
        RedactResult result;
        result.Family = DocumentFamily::Word;

        auto document = editor.GetDocument();
        if (!document)
        {
            AddError(result.Diagnostics, "Word document has no package");
            return result;
        }
        OpenXmlPackage& package = *document;

        if (options.RemoveComments)
        {
            result.CommentsRemoved = editor.Comments().size();
        }

        // Which styles hide text has to be known before any part is scrubbed,
        // because the answer lives in a different part from the runs it hides.
        // A document can hold more than one styles part - the main one and a
        // glossary one - and the same `styleId` in each is a different style, so
        // the sets are kept per graph rather than unioned across both: unioning
        // would apply a glossary style's hiddenness to a main-document run that
        // merely shares its id, deleting text the document shows. Within one
        // graph the ids from every styles part are unioned, so a style that
        // hides text is never missed because another part came later.
        std::unordered_set<std::string> mainHiddenStyles;
        std::unordered_set<std::string> glossaryHiddenStyles;
        if (options.RemoveHiddenText)
        {
            for (const auto& record : ListParts(package))
            {
                if (!IsWordStylesContentType(record.ContentType))
                {
                    continue;
                }
                if (const auto part = package.GetPartByUri(record.Uri))
                {
                    auto& scope = IsGlossaryUri(record.Uri) ? glossaryHiddenStyles : mainHiddenStyles;
                    scope.merge(HiddenStyleIds(openxmlelement_cast<W::Styles>(part->GetRootElement())));
                }
            }
        }

        // One pass per story part, over the typed tree the part already holds.
        // Revisions are accepted here rather than through the typed editor
        // because that one walks the main document body only: a tracked deletion
        // in a header, a footnote or a comment would survive it, and "the
        // reviewed text" would be published together with the text the review
        // removed.
        if (options.RemoveComments || options.RemoveHiddenText || options.ResolveRevisions ||
            options.RemovePersonalMetadata)
        {
            Size commentMarkersRemoved = 0;
            for (const auto& record : ListParts(package))
            {
                if (!IsWordStoryContentType(record.ContentType))
                {
                    continue;
                }
                if (options.RemoveComments && IsWordCommentContentType(record.ContentType))
                {
                    // The whole part goes below, so scrubbing it first would only
                    // count revisions that are about to be deleted with it.
                    continue;
                }
                auto part = package.GetPartByUri(record.Uri);
                auto root = part ? part->GetRootElement() : nullptr;
                if (!root)
                {
                    continue;
                }

                if (options.ResolveRevisions)
                {
                    // Structure first: both of these need the `w:del` markers
                    // that accepting the changes then removes.
                    Size resolved = MergeDeletedParagraphMarks(root);
                    resolved += RemoveDeletedTableRows(root);
                    AcceptRevisions(root, resolved);
                    result.RevisionsResolved += resolved;
                }

                if (options.RemoveComments || options.RemoveHiddenText)
                {
                    // A glossary story part is scrubbed with the glossary styles,
                    // a main-document one with the main styles, so a shared
                    // `styleId` is judged against the styles that actually apply.
                    const auto& hiddenStyles =
                        IsGlossaryUri(record.Uri) ? glossaryHiddenStyles : mainHiddenStyles;
                    ScrubWordPart(root, options.RemoveComments, options.RemoveHiddenText, hiddenStyles,
                                  commentMarkersRemoved, result.HiddenRunsRemoved);
                }

                if (options.RemovePersonalMetadata)
                {
                    RemoveRevisionSaveIds(root);
                }
            }
        }

        if (options.RemoveComments)
        {
            for (const auto& record : ListParts(package))
            {
                if (IsWordCommentContentType(record.ContentType))
                {
                    DetachPart(package, record, result.Diagnostics);
                }
            }
        }

        if (options.RemovePersonalMetadata)
        {
            for (const auto& record : ListParts(package))
            {
                if (!IsWordSettingsContentType(record.ContentType))
                {
                    continue;
                }
                if (const auto part = package.GetPartByUri(record.Uri))
                {
                    ScrubWordSettings(part->GetRootElement());
                }
            }
            RedactMetadata(package, result, options.RemoveComments);
        }

        result.Ok = true;
        return result;
    }

    static RedactResult RedactExcel(Excel::ExcelDocumentEditor& editor, const RedactOptions& options)
    {
        RedactResult result;
        result.Family = DocumentFamily::Excel;

        if (options.RemoveComments)
        {
            for (const auto& worksheet : editor.Worksheets())
            {
                for (const auto& comment : worksheet->Comments())
                {
                    if (worksheet->RemoveComment(comment.Address))
                    {
                        ++result.CommentsRemoved;
                    }
                }
                for (const auto& threaded : worksheet->ThreadedComments())
                {
                    if (worksheet->RemoveThreadedComment(threaded.Id))
                    {
                        ++result.CommentsRemoved;
                    }
                }
            }

            // The person registry outlives the threaded comments that referred
            // to it, and every entry in it names someone.
            if (auto* document = editor.GetDocument().get())
            {
                for (const auto& record : ListParts(*document))
                {
                    if (record.ContentType.ends_with("person+xml") &&
                        DetachPart(*document, record, result.Diagnostics))
                    {
                        AddInfo(result.Diagnostics, "Threaded comment person registry removed", record.Uri);
                        ++result.PartsRemoved;
                    }
                }
            }
        }

        if (options.RemovePersonalMetadata)
        {
            RedactMetadata(*editor.GetDocument(), result, options.RemoveComments);
        }

        result.Ok = true;
        return result;
    }

    static RedactResult RedactPowerPoint(PowerPoint::PowerPointDocumentEditor& editor, const RedactOptions& options)
    {
        RedactResult result;
        result.Family = DocumentFamily::PowerPoint;

        if (options.RemoveComments)
        {
            for (const auto& slide : editor.Slides())
            {
                for (const auto& comment : slide->Comments())
                {
                    if (slide->RemoveComment(comment.Id))
                    {
                        ++result.CommentsRemoved;
                    }
                }
            }
            // Authors are comment metadata; with the comments gone they are
            // removable, and each one names a person.
            for (const auto& author : editor.CommentAuthors())
            {
                editor.RemoveCommentAuthor(author.Id);
            }

            // The typed API covers the modern comment parts. A presentation
            // written by an older PowerPoint keeps its comments in
            // `ppt/comments/*.xml` instead, where nothing above would find them,
            // so those parts are detached whole.
            if (auto* document = editor.GetDocument().get())
            {
                for (const auto& record : ListParts(*document))
                {
                    if ((IsLegacyPowerPointCommentPart(record) ||
                         record.ContentType.ends_with("commentAuthors+xml")) &&
                        DetachPart(*document, record, result.Diagnostics))
                    {
                        AddInfo(result.Diagnostics, "Legacy PowerPoint comment part removed", record.Uri);
                        ++result.PartsRemoved;
                        ++result.CommentsRemoved;
                    }
                }
            }
        }

        if (options.RemovePersonalMetadata)
        {
            RedactMetadata(*editor.GetDocument(), result, options.RemoveComments);
        }

        result.Ok = true;
        return result;
    }

    /// Writes a redaction result out, unless the redaction itself already failed.
    static RedactResult SaveRedacted(RedactResult result, OpenXmlPackage* package, const std::filesystem::path& destination,
                                     const char* failureMessage)
    {
        // Here because all three families reach it with the package in hand, and
        // ahead of the Ok check because a redaction that failed still has to say
        // what it never saw.
        CarryLoadDiagnostics(result, package);

        if (!result.Ok)
        {
            return result;
        }

        if (package == nullptr || !package->SaveToFile(destination))
        {
            result.Ok = false;
            AddError(result.Diagnostics, failureMessage, destination.string());
            return result;
        }

        result.Saved = true;
        return result;
    }
};

RedactResult RedactDocument(const std::filesystem::path& path, const std::filesystem::path& outputPath,
                            const RedactOptions& options)
{
    RedactResult result;

    OpenXmlPackage probe;
    ApplyDefaultPackageLimits(probe);
    if (!probe.LoadFromFile(path))
    {
        DocumentRedactorHelper::AddError(result.Diagnostics, "Failed to open package", path.string());
        return result;
    }

    const auto destination = outputPath.empty() ? path : outputPath;
    const auto info = GetInfo(probe);
    switch (info.Family)
    {
        case DocumentFamily::Word:
        {
            auto editor = Word::WordDocumentEditor::Open(path, UntrustedOpenSettings());
            if (!editor)
            {
                DocumentRedactorHelper::AddError(result.Diagnostics, "Failed to open Word document", path.string());
                return result;
            }
            result = DocumentRedactorHelper::RedactWord(*editor, options);
            // Saved through the package: DocumentRedactorHelper::RedactWord works over the parts
            // directly once the typed edits are done, so the editor's own
            // save would not see them.
            return DocumentRedactorHelper::SaveRedacted(result, editor->GetDocument().get(), destination, "Failed to save document");
        }
        case DocumentFamily::Excel:
        {
            auto editor = Excel::ExcelDocumentEditor::Open(path, UntrustedOpenSettings());
            if (!editor)
            {
                DocumentRedactorHelper::AddError(result.Diagnostics, "Failed to open Excel document", path.string());
                return result;
            }
            result = DocumentRedactorHelper::RedactExcel(*editor, options);
            return DocumentRedactorHelper::SaveRedacted(result, editor->GetDocument().get(), destination, "Failed to save workbook");
        }
        case DocumentFamily::PowerPoint:
        {
            auto editor = PowerPoint::PowerPointDocumentEditor::Open(path, UntrustedOpenSettings());
            if (!editor)
            {
                DocumentRedactorHelper::AddError(result.Diagnostics, "Failed to open PowerPoint document", path.string());
                return result;
            }
            result = DocumentRedactorHelper::RedactPowerPoint(*editor, options);
            return DocumentRedactorHelper::SaveRedacted(result, editor->GetDocument().get(), destination, "Failed to save presentation");
        }
        case DocumentFamily::Unknown:
            break;
    }

    DocumentRedactorHelper::AddError(result.Diagnostics, DescribeUnknownFamily(info), path.string());
    return result;
}

// The overloads over an already open document do not save, so they do not pass
// through SaveRedacted and carry the loader's diagnostics themselves.

RedactResult RedactDocument(Word::WordDocumentEditor& editor, const RedactOptions& options)
{
    auto result = DocumentRedactorHelper::RedactWord(editor, options);
    DocumentRedactorHelper::CarryLoadDiagnostics(result, editor.GetDocument().get());
    return result;
}

RedactResult RedactDocument(Excel::ExcelDocumentEditor& editor, const RedactOptions& options)
{
    auto result = DocumentRedactorHelper::RedactExcel(editor, options);
    DocumentRedactorHelper::CarryLoadDiagnostics(result, editor.GetDocument().get());
    return result;
}

RedactResult RedactDocument(PowerPoint::PowerPointDocumentEditor& editor, const RedactOptions& options)
{
    auto result = DocumentRedactorHelper::RedactPowerPoint(editor, options);
    DocumentRedactorHelper::CarryLoadDiagnostics(result, editor.GetDocument().get());
    return result;
}

} // namespace ExyokiOffice::Tools
