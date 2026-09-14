// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/OpenXmlPackageValidator.hpp"

#include "ExyokiOffice/OpenXmlDomValidator.hpp"
#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/OpenXMLElement.hpp"
#include "ExyokiOffice/Excel/ExcelAddress.hpp"
#include "ExyokiOffice/Packaging/GeneratedSchematron.hpp"
#include "ExyokiOffice/Packaging/GeneratedParts.hpp"
#include "ConformanceClass.hpp"
#include "OpenXmlDomInternal.hpp"
#include "OpenXmlPackageInternal.hpp"
#include "OpenXmlPackageUri.hpp"
#include "OpenXmlVersionSupport.hpp"
#include "XmlNamespaceResolver.hpp"
#include "XmlParseOptions.hpp"
#include "ExyokiOffice/StandardTypes.hpp"
#include "AsciiText.hpp"

#include <algorithm>
#include <charconv>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ExyokiOffice
{

using Detail::SupportsOfficeVersion;

/**
 * @brief The package-semantic rules Office enforces beyond OPC structure and the schema.
 *
 * Each rule here was found by watching Word, Excel or PowerPoint refuse a file
 * that validated clean: a relationship reference no rule covered, two tables
 * over the same cells, a threaded comment whose author is not in the person
 * list, a slide without the master/layout/theme chain, and - accepted by Word
 * but shown as Normal - a style reference nothing defines. They run in both
 * validator configurations, because they are about the package rather than
 * about the markup of one part. docs/tools/exyoki.md lists them.
 */
class OpenXmlPackageSemanticRulesHelper
{
public:
    static constexpr std::string_view kRelationshipsNamespace =
        "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
    static constexpr std::string_view kSpreadsheetNamespace =
        "http://schemas.openxmlformats.org/spreadsheetml/2006/main";
    static constexpr std::string_view kThreadedCommentsNamespace =
        "http://schemas.microsoft.com/office/spreadsheetml/2018/threadedcomments";
    static constexpr std::string_view kPresentationNamespace =
        "http://schemas.openxmlformats.org/presentationml/2006/main";
    static constexpr std::string_view kWordprocessingNamespace =
        "http://schemas.openxmlformats.org/wordprocessingml/2006/main";

    static bool HasRelationship(const OpenXmlPartContainer& container, std::string_view id)
    {
        const auto& relationships = container.Relationships();
        return std::any_of(relationships.begin(), relationships.end(), [id](const OpenXmlRelationship& relationship)
                           { return relationship.Id == id; });
    }

    static bool HasRelationshipOfType(const OpenXmlPartContainer& container, std::string_view type)
    {
        const auto& relationships = container.Relationships();
        return std::any_of(relationships.begin(), relationships.end(), [type](const OpenXmlRelationship& relationship)
                           { return relationship.Type == type; });
    }

    static std::shared_ptr<OpenXmlPackagePart> FirstChildPartNamed(const OpenXmlPartContainer& container,
                                                                   std::string_view descriptorName)
    {
        for (const auto& child : container.Parts())
        {
            if (child && child->Descriptor().Name == descriptorName)
            {
                return child;
            }
        }
        return nullptr;
    }

    template <typename TCallback>
    static void VisitElements(const std::shared_ptr<OpenXMLElement>& element, TCallback&& callback)
    {
        if (!element)
        {
            return;
        }

        callback(*element);
        for (const auto& child : element->Children())
        {
            VisitElements(child, callback);
        }
    }

    static ValidationIssue MakeElementIssue(ValidationErrorId id,
                                            ValidationSeverity severity,
                                            std::string message,
                                            const OpenXmlPackagePart& part,
                                            XmlLocation location)
    {
        ValidationIssue issue;
        issue.Severity = severity;
        issue.Domain = ValidationDomain::Packaging;
        issue.Id = id;
        issue.Message = std::move(message);
        issue.Location = std::move(location);
        issue.PartUri = part.Uri();
        return issue;
    }

    static ValidationIssue MakeDanglingRelationshipReferenceIssue(const OpenXmlPackagePart& part,
                                                                  const OpenXMLElement& element,
                                                                  const OpenXmlQualifiedName& attribute,
                                                                  std::string_view relationshipId)
    {
        auto location = element.GetXmlLocation(attribute);
        auto issue = MakeElementIssue(ValidationErrorId::PackageDanglingRelationshipReference,
                                      ValidationSeverity::Error,
                                      "Element '" + location.ElementName + "' attribute '" + location.AttributeName +
                                          "' references relationship '" + std::string(relationshipId) +
                                          "', which part '" + part.Uri() + "' does not define.",
                                      part,
                                      std::move(location));
        issue.RelationshipSourceUri = part.Uri();
        issue.RelationshipId = std::string(relationshipId);
        return issue;
    }

    /**
     * @brief Checks every relationship-namespace attribute of @p element.
     *
     * The generated schematron rules know only the elements the SDK metadata
     * lists, and `x:tablePart` is not among them: a worksheet whose table
     * reference pointed nowhere validated clean while Excel refused it. What
     * makes an attribute a relationship reference is its namespace, not the
     * element it sits on, so the attributes are read from the node itself -
     * the typed DOM exposes only what its schema declares - and each prefix is
     * resolved through the bindings in scope, whatever prefix the document
     * chose. Attributes a schematron rule already checked are skipped so a
     * missing relationship is reported once.
     */
    static void ValidateRelationshipAttributes(const OpenXmlPackagePart& part,
                                               const OpenXMLElement& element,
                                               std::span<const OpenXmlQualifiedName> alreadyChecked,
                                               DiagnosticSink& sink)
    {
        const auto node = Detail::ToNode(element.GetNodeHandle());
        if (!node)
        {
            return;
        }

        for (auto attribute = node.first_attribute(); attribute; attribute = attribute.next_attribute())
        {
            const std::string_view name = attribute.name();
            const auto colon = name.find(':');
            if (colon == std::string_view::npos)
            {
                continue;
            }

            const auto prefix = name.substr(0, colon);
            const std::string_view value = attribute.value();
            if (prefix == "xmlns" || value.empty())
            {
                continue;
            }

            const auto uri = Xml::NamespaceResolver::LookupUriForPrefix(node, prefix);
            if (!uri || *uri != kRelationshipsNamespace)
            {
                continue;
            }

            const OpenXmlQualifiedName attributeName(kRelationshipsNamespace, name.substr(colon + 1));
            if (std::find(alreadyChecked.begin(), alreadyChecked.end(), attributeName) != alreadyChecked.end() ||
                HasRelationship(part, value))
            {
                continue;
            }

            sink.Report(MakeDanglingRelationshipReferenceIssue(part, element, attributeName, value));
        }
    }

    struct TableReference
    {
        std::shared_ptr<OpenXmlPackagePart> Part;
        Excel::CellRange Range;
        std::string Text;
    };

    static bool RangesOverlap(const Excel::CellRange& left, const Excel::CellRange& right) noexcept
    {
        return left.First().Row().Value() <= right.Last().Row().Value() &&
               right.First().Row().Value() <= left.Last().Row().Value() &&
               left.First().Column().Value() <= right.Last().Column().Value() &&
               right.First().Column().Value() <= left.Last().Column().Value();
    }

    /// Excel refuses a worksheet two of whose tables share a cell.
    static void ValidateWorksheetTables(const OpenXmlPackagePart& worksheet, DiagnosticSink& sink)
    {
        const OpenXmlQualifiedName tableName(kSpreadsheetNamespace, "table");
        const OpenXmlQualifiedName referenceAttribute(std::string_view{}, "ref");

        std::vector<TableReference> tables;
        for (const auto& child : worksheet.Parts())
        {
            if (!child || child->Descriptor().Name != "TableDefinitionPart" || !child->IsXmlPart())
            {
                continue;
            }

            const auto root = child->GetRootElement();
            std::string_view text;
            if (!root || root->QualifiedName() != tableName || !root->TryGetAttribute(referenceAttribute, text))
            {
                continue;
            }

            // A reference that does not parse is the schema's to report.
            const auto range = Excel::CellRange::ParseA1(text);
            if (range)
            {
                tables.push_back({child, *range, std::string(text)});
            }
        }

        for (Size first = 0; first < tables.size(); ++first)
        {
            for (Size second = first + 1; second < tables.size(); ++second)
            {
                if (!RangesOverlap(tables[first].Range, tables[second].Range))
                {
                    continue;
                }

                auto issue = MakeElementIssue(ValidationErrorId::PackageTableRangeOverlap,
                                              ValidationSeverity::Error,
                                              "Table part '" + tables[first].Part->Uri() + "' (" + tables[first].Text +
                                                  ") and table part '" + tables[second].Part->Uri() + "' (" +
                                                  tables[second].Text + ") of worksheet '" + worksheet.Uri() +
                                                  "' overlap; Excel refuses a worksheet whose tables share cells.",
                                              worksheet,
                                              {});
                issue.TargetUri = tables[second].Part->Uri();
                sink.Report(std::move(issue));
            }
        }
    }

    static std::unordered_set<std::string> CollectWorkbookPersonIds(const OpenXmlPackage& package, bool& hasPersonList)
    {
        const OpenXmlQualifiedName personName(kThreadedCommentsNamespace, "person");
        const OpenXmlQualifiedName idAttribute(std::string_view{}, "id");

        std::unordered_set<std::string> ids;
        hasPersonList = false;
        for (const auto& root : package.Parts())
        {
            if (!root || root->Descriptor().Name != "WorkbookPart")
            {
                continue;
            }

            for (const auto& child : root->Parts())
            {
                if (!child || child->Descriptor().Name != "WorkbookPersonPart" || !child->IsXmlPart())
                {
                    continue;
                }

                hasPersonList = true;
                VisitElements(child->GetRootElement(), [&](const OpenXMLElement& element)
                              {
                    std::string_view id;
                    if (element.QualifiedName() == personName && element.TryGetAttribute(idAttribute, id))
                    {
                        ids.insert(std::string(id));
                    } });
            }
        }
        return ids;
    }

    /**
     * @brief Excel refuses a threaded comment whose author the workbook does not list.
     *
     * The person list hangs off the workbook part, not off the worksheet the
     * comment belongs to, which is exactly why importing a sheet loses it. One
     * issue per unknown person per part: a thread of twenty replies by the
     * same author is one defect, not twenty.
     */
    static void ValidateThreadedCommentPersons(const OpenXmlPackage& package,
                                               const OpenXmlPackagePart& part,
                                               const std::shared_ptr<OpenXMLElement>& root,
                                               DiagnosticSink& sink)
    {
        const OpenXmlQualifiedName commentName(kThreadedCommentsNamespace, "threadedComment");
        const OpenXmlQualifiedName personAttribute(std::string_view{}, "personId");

        bool hasPersonList = false;
        const auto persons = CollectWorkbookPersonIds(package, hasPersonList);
        std::unordered_set<std::string> reported;
        VisitElements(root, [&](const OpenXMLElement& element)
                      {
            std::string_view personId;
            if (element.QualifiedName() != commentName || !element.TryGetAttribute(personAttribute, personId) ||
                personId.empty() || persons.contains(std::string(personId)) ||
                !reported.insert(std::string(personId)).second)
            {
                return;
            }

            std::string message = "Threaded comment names person '" + std::string(personId) + "', which ";
            message += hasPersonList ? "no person list related from the workbook defines."
                                     : "the workbook cannot resolve: it has no person list part.";
            message += " Excel refuses the workbook.";
            sink.Report(MakeElementIssue(ValidationErrorId::PackageThreadedCommentPersonUndefined,
                                         ValidationSeverity::Error,
                                         std::move(message),
                                         part,
                                         element.GetXmlLocation(personAttribute))); });
    }

    /// PowerPoint refuses a presentation that does not list at least one slide master.
    static void ValidatePresentationMasters(const OpenXmlPackagePart& part,
                                            const std::shared_ptr<OpenXMLElement>& root,
                                            DiagnosticSink& sink)
    {
        const OpenXmlQualifiedName presentationName(kPresentationNamespace, "presentation");
        const OpenXmlQualifiedName listName(kPresentationNamespace, "sldMasterIdLst");
        const OpenXmlQualifiedName entryName(kPresentationNamespace, "sldMasterId");

        bool listed = false;
        if (root && root->QualifiedName() == presentationName)
        {
            for (const auto& child : root->Children())
            {
                if (!child || child->QualifiedName() != listName)
                {
                    continue;
                }
                for (const auto& entry : child->Children())
                {
                    if (entry && entry->QualifiedName() == entryName)
                    {
                        listed = true;
                    }
                }
            }
        }

        const bool related = HasRelationshipOfType(part, Packaging::SlideMasterPart::Descriptor().RelationshipType);
        if (listed && related)
        {
            return;
        }

        std::string message = "Presentation part '" + part.Uri() + "' has no slide master: ";
        if (!listed && !related)
        {
            message += "p:presentation carries no p:sldMasterIdLst entry and the part has no slideMaster relationship.";
        }
        else if (!listed)
        {
            message += "p:presentation carries no p:sldMasterIdLst entry for the related slide master part.";
        }
        else
        {
            message += "p:sldMasterIdLst names a master but the part has no slideMaster relationship.";
        }
        message += " PowerPoint refuses a presentation without a slide master.";
        sink.Report(MakeElementIssue(ValidationErrorId::PackagePresentationMissingSlideMaster,
                                     ValidationSeverity::Error,
                                     std::move(message),
                                     part,
                                     root ? root->GetXmlLocation() : XmlLocation{}));
    }

    /// The slide -> layout -> master -> theme chain PowerPoint requires of every slide.
    static void RequireRelationship(const OpenXmlPackagePart& part,
                                    std::string_view relationshipType,
                                    ValidationErrorId id,
                                    std::string_view targetDescription,
                                    DiagnosticSink& sink)
    {
        if (HasRelationshipOfType(part, relationshipType))
        {
            return;
        }

        sink.Report(MakeElementIssue(id,
                                     ValidationSeverity::Error,
                                     "Part '" + part.Uri() + "' has no " + std::string(targetDescription) +
                                         " relationship (" + std::string(relationshipType) +
                                         "); PowerPoint refuses the presentation.",
                                     part,
                                     {}));
    }

    static bool IsWordContentPart(std::string_view descriptorName) noexcept
    {
        return descriptorName == "MainDocumentPart" || descriptorName == "HeaderPart" ||
               descriptorName == "FooterPart" || descriptorName == "FootnotesPart" ||
               descriptorName == "EndnotesPart" || descriptorName == "WordprocessingCommentsPart" ||
               descriptorName == "GlossaryDocumentPart";
    }

    /**
     * The styles part is owned by the main document part or by the glossary
     * part, so a header or a footnotes part reaches it through whichever part
     * relates it. Looking through the parents rather than at the main part
     * alone keeps the glossary's own styles with the glossary's content.
     */
    static std::shared_ptr<OpenXmlPackagePart> FindStylesPart(const OpenXmlPackage& package,
                                                              const OpenXmlPackagePart& part)
    {
        if (auto own = FirstChildPartNamed(part, "StyleDefinitionsPart"))
        {
            return own;
        }

        for (const auto& incoming : part.IncomingRelationships())
        {
            if (incoming.SourceUri == "/")
            {
                continue;
            }
            if (const auto parent = package.GetPartByUri(incoming.SourceUri))
            {
                if (auto styles = FirstChildPartNamed(*parent, "StyleDefinitionsPart"))
                {
                    return styles;
                }
            }
        }
        return nullptr;
    }

    /**
     * @brief Warns about `w:pStyle`, `w:rStyle` and `w:tblStyle` references nothing defines.
     *
     * A warning rather than an error, because Word opens the file; it just
     * shows the paragraph in the Normal style, which turns headings into body
     * text without a word. Reported once per style id per part.
     */
    static void ValidateStyleReferences(const OpenXmlPackage& package,
                                        const OpenXmlPackagePart& part,
                                        const std::shared_ptr<OpenXMLElement>& root,
                                        DiagnosticSink& sink)
    {
        const OpenXmlQualifiedName styleName(kWordprocessingNamespace, "style");
        const OpenXmlQualifiedName styleIdAttribute(kWordprocessingNamespace, "styleId");
        const OpenXmlQualifiedName valueAttribute(kWordprocessingNamespace, "val");

        const auto stylesPart = FindStylesPart(package, part);
        std::unordered_set<std::string> defined;
        if (stylesPart && stylesPart->IsXmlPart())
        {
            VisitElements(stylesPart->GetRootElement(), [&](const OpenXMLElement& element)
                          {
                std::string_view id;
                if (element.QualifiedName() == styleName && element.TryGetAttribute(styleIdAttribute, id))
                {
                    defined.insert(std::string(id));
                } });
        }

        std::unordered_set<std::string> reported;
        VisitElements(root, [&](const OpenXMLElement& element)
                      {
            const auto name = element.QualifiedName();
            if (name.namespaceUri() != kWordprocessingNamespace)
            {
                return;
            }
            const auto local = name.localName();
            if (local != "pStyle" && local != "rStyle" && local != "tblStyle")
            {
                return;
            }

            std::string_view styleId;
            if (!element.TryGetAttribute(valueAttribute, styleId) || styleId.empty() ||
                defined.contains(std::string(styleId)) || !reported.insert(std::string(styleId)).second)
            {
                return;
            }

            auto location = element.GetXmlLocation(valueAttribute);
            std::string message = "Element '" + location.ElementName + "' references style '" + std::string(styleId) +
                                  "', which ";
            message += stylesPart ? "the styles part '" + stylesPart->Uri() + "' does not define."
                                  : "cannot resolve: the document has no styles part.";
            message += " Word shows such content in the Normal style. Reported once per style id and part.";
            sink.Report(MakeElementIssue(ValidationErrorId::PackageStyleReferenceUndefined,
                                         ValidationSeverity::Warning,
                                         std::move(message),
                                         part,
                                         std::move(location))); });
    }

    static void ValidatePart(const OpenXmlPackage& package,
                             const OpenXmlPackagePart& part,
                             const std::shared_ptr<OpenXMLElement>& root,
                             DiagnosticSink& sink)
    {
        const auto name = part.Descriptor().Name;
        if (name == "WorksheetPart")
        {
            ValidateWorksheetTables(part, sink);
        }
        else if (name == "WorksheetThreadedCommentsPart")
        {
            ValidateThreadedCommentPersons(package, part, root, sink);
        }
        else if (name == "PresentationPart")
        {
            ValidatePresentationMasters(part, root, sink);
        }
        else if (name == "SlidePart")
        {
            RequireRelationship(part,
                                Packaging::SlideLayoutPart::Descriptor().RelationshipType,
                                ValidationErrorId::PackageSlideMissingSlideLayout,
                                "slide layout",
                                sink);
        }
        else if (name == "SlideLayoutPart")
        {
            RequireRelationship(part,
                                Packaging::SlideMasterPart::Descriptor().RelationshipType,
                                ValidationErrorId::PackageSlideLayoutMissingSlideMaster,
                                "slide master",
                                sink);
        }
        else if (name == "SlideMasterPart")
        {
            RequireRelationship(part,
                                Packaging::ThemePart::Descriptor().RelationshipType,
                                ValidationErrorId::PackageSlideMasterMissingTheme,
                                "theme",
                                sink);
        }
        else if (IsWordContentPart(name))
        {
            ValidateStyleReferences(package, part, root, sink);
        }
    }
};

/// File-local rule helpers behind package validation.
class OpenXmlPackageValidatorHelper
{
public:
    class PartDiagnosticSink final : public DiagnosticSink
    {
    public:
        PartDiagnosticSink(DiagnosticSink& target, std::string_view partUri)
            : target_(target), partUri_(partUri)
        {
        }

        void Report(ValidationIssue issue) override
        {
            if (issue.PartUri.empty())
            {
                issue.PartUri = partUri_;
            }
            target_.Report(std::move(issue));
        }

    private:
        DiagnosticSink& target_;
        std::string partUri_;
    };

    static ValidationIssue MakeOpcIssue(ValidationErrorId id,
                                        std::string message,
                                        std::string_view sourceUri,
                                        const OpenXmlRelationship& relationship)
    {
        ValidationIssue issue;
        issue.Severity = ValidationSeverity::Error;
        issue.Domain = ValidationDomain::Opc;
        issue.Id = id;
        issue.Message = std::move(message);
        issue.PartUri = std::string(sourceUri);
        issue.RelationshipSourceUri = std::string(sourceUri);
        issue.RelationshipId = relationship.Id;
        issue.RelationshipType = relationship.Type;
        issue.TargetUri = relationship.Target;
        issue.TargetMode = relationship.TargetMode;
        return issue;
    }

    static ValidationIssue MakePackageSchematronIssue(std::string message,
                                                      const OpenXmlPackagePart& part,
                                                      const OpenXMLElement& element,
                                                      const OpenXmlRelationship& relationship,
                                                      std::string_view expectedRelationshipType)
    {
        ValidationIssue issue;
        issue.Severity = ValidationSeverity::Error;
        issue.Domain = ValidationDomain::Packaging;
        issue.Id = ValidationErrorId::Unknown;
        issue.Message = std::move(message);
        issue.Location = element.GetXmlLocation();
        issue.PartUri = part.Uri();
        issue.RelationshipSourceUri = part.Uri();
        issue.RelationshipId = relationship.Id;
        issue.RelationshipType = relationship.Type;
        issue.TargetUri = relationship.Target;
        if (!expectedRelationshipType.empty())
        {
            issue.Message += " Expected relationship type '" + std::string(expectedRelationshipType) + "'.";
        }
        return issue;
    }

    static ValidationIssue MakePackageSchematronElementIssue(std::string message,
                                                             const OpenXmlPackagePart& part,
                                                             const OpenXMLElement& element,
                                                             std::string_view testExpression)
    {
        ValidationIssue issue;
        issue.Severity = ValidationSeverity::Error;
        issue.Domain = ValidationDomain::Packaging;
        issue.Id = ValidationErrorId::Unknown;
        issue.Message = std::move(message);
        if (!testExpression.empty())
        {
            issue.Message += " Schematron test: " + std::string(testExpression) + ".";
        }
        issue.Location = element.GetXmlLocation();
        issue.PartUri = part.Uri();
        return issue;
    }

    static const OpenXmlRelationship* FindRelationshipById(const OpenXmlPartContainer& container, std::string_view id)
    {
        for (const auto& relationship : container.Relationships())
        {
            if (relationship.Id == id)
            {
                return &relationship;
            }
        }
        return nullptr;
    }

    static std::string NormalizeSchematronUniqueValue(std::string_view value, bool caseInsensitive)
    {
        return caseInsensitive ? AsciiText::ToLower(value) : std::string(value);
    }

    template <typename TCallback>
    static void VisitOpenXmlTree(const std::shared_ptr<OpenXMLElement>& element, TCallback&& callback)
    {
        if (!element)
        {
            return;
        }

        callback(*element);
        // Children are typed from the parent's content model: several Open XML
        // names belong to more than one type - `w:bottom` is a border in `w:pBdr`
        // and a width in `w:tblCellMar` - and the schematron rules of the wrong
        // type would reject valid markup.
        for (const auto& child : element->ChildrenInContentModel())
        {
            VisitOpenXmlTree(child, callback);
        }
    }

    static bool MatchesElementPath(const OpenXMLElement& element, std::span<const OpenXmlQualifiedName> path)
    {
        if (path.empty() || element.QualifiedName() != path.back())
        {
            return false;
        }

        auto current = element.Parent();
        for (Size index = path.size() - 1; index > 0; --index)
        {
            if (!current || current->QualifiedName() != path[index - 1])
            {
                return false;
            }
            current = current->Parent();
        }

        return true;
    }

    static void ResolvePartSelectorPath(const std::vector<std::shared_ptr<OpenXmlPackagePart>>& candidates,
                                        std::span<const std::string_view> path,
                                        std::vector<std::shared_ptr<OpenXmlPackagePart>>& result)
    {
        if (path.empty())
        {
            result.insert(result.end(), candidates.begin(), candidates.end());
            return;
        }

        std::vector<std::shared_ptr<OpenXmlPackagePart>> next;
        for (const auto& candidate : candidates)
        {
            if (!candidate)
            {
                continue;
            }

            if (candidate->Descriptor().Name == path.front())
            {
                if (path.size() == 1)
                {
                    next.push_back(candidate);
                }
                else
                {
                    ResolvePartSelectorPath(candidate->Parts(), path.subspan(1), result);
                }
            }
        }

        if (path.size() == 1)
        {
            result.insert(result.end(), next.begin(), next.end());
        }
    }

    static std::vector<std::shared_ptr<OpenXmlPackagePart>> ResolvePackageSchematronPartSelector(
        const OpenXmlPackage& package,
        const OpenXmlPackagePart& currentPart,
        const Generated::PackageSchematronPartSelector& selector)
    {
        switch (selector.Kind)
        {
            case Generated::PackageSchematronPartSelectorKind::CurrentPart:
                return {package.GetPartByUri(currentPart.Uri())};
            case Generated::PackageSchematronPartSelectorKind::ParentPart:
            {
                std::vector<std::shared_ptr<OpenXmlPackagePart>> parents;
                for (const auto& incoming : currentPart.IncomingRelationships())
                {
                    if (incoming.SourceUri == "/")
                    {
                        continue;
                    }
                    if (auto parent = package.GetPartByUri(incoming.SourceUri))
                    {
                        parents.push_back(parent);
                    }
                }
                return parents;
            }
            case Generated::PackageSchematronPartSelectorKind::AbsolutePartPath:
            {
                std::vector<std::shared_ptr<OpenXmlPackagePart>> result;
                ResolvePartSelectorPath(package.Parts(), selector.Path, result);
                return result;
            }
            case Generated::PackageSchematronPartSelectorKind::RelativePartPath:
            {
                std::vector<std::shared_ptr<OpenXmlPackagePart>> result;
                ResolvePartSelectorPath(currentPart.Parts(), selector.Path, result);
                return result;
            }
        }
        return {};
    }

    /// Reads a number out of document markup, which is always in the invariant
    /// XML spelling. std::strtod would read it through the global C locale, so
    /// the same document would fail this validation under de-DE and pass under
    /// en-US - reported as a defect in the document rather than in the reader.
    static bool TryParseSchematronNumber(std::string_view text, Real& value)
    {
        const auto* const end = text.data() + text.size();
        const auto result = std::from_chars(text.data(), end, value, std::chars_format::general);
        return result.ec == std::errc{} && result.ptr == end;
    }

    static bool EvaluateSchematronComparison(Real lhs,
                                             Generated::PackageSchematronComparisonOperator comparison,
                                             Real rhs) noexcept
    {
        switch (comparison)
        {
            case Generated::PackageSchematronComparisonOperator::LessThan:
                return lhs < rhs;
            case Generated::PackageSchematronComparisonOperator::LessThanOrEqual:
                return lhs <= rhs;
            case Generated::PackageSchematronComparisonOperator::GreaterThan:
                return lhs > rhs;
            case Generated::PackageSchematronComparisonOperator::GreaterThanOrEqual:
                return lhs >= rhs;
        }
        return false;
    }

    static std::string_view PackageSchematronComparisonOperatorText(
        Generated::PackageSchematronComparisonOperator comparison) noexcept
    {
        switch (comparison)
        {
            case Generated::PackageSchematronComparisonOperator::LessThan:
                return "<";
            case Generated::PackageSchematronComparisonOperator::LessThanOrEqual:
                return "<=";
            case Generated::PackageSchematronComparisonOperator::GreaterThan:
                return ">";
            case Generated::PackageSchematronComparisonOperator::GreaterThanOrEqual:
                return ">=";
        }
        return "?";
    }

    static void CollectPackageSchematronAttributeValues(const OpenXmlPackagePart& targetPart,
                                                        std::span<const OpenXmlQualifiedName> elementPath,
                                                        const OpenXmlQualifiedName& attributeName,
                                                        std::unordered_set<std::string>& values)
    {
        if (!targetPart.IsXmlPart())
        {
            return;
        }

        VisitOpenXmlTree(targetPart.GetRootElement(), [&](const OpenXMLElement& element)
                         {
            if (MatchesElementPath(element, elementPath))
            {
                std::string_view value;
                if (element.TryGetAttribute(attributeName, value))
                {
                    values.insert(std::string(value));
                }
            } });
    }

    static Size CountPackageSchematronElements(const OpenXmlPackagePart& targetPart,
                                               std::span<const OpenXmlQualifiedName> elementPath)
    {
        if (!targetPart.IsXmlPart())
        {
            return 0;
        }

        Size count = 0;
        VisitOpenXmlTree(targetPart.GetRootElement(), [&](const OpenXMLElement& element)
                         {
            if (MatchesElementPath(element, elementPath))
            {
                ++count;
            } });
        return count;
    }

    static void ValidatePackageSchematronElement(const OpenXmlPackagePart& part,
                                                 const OpenXMLElement& element,
                                                 DiagnosticSink& sink)
    {
        const auto elementName = element.QualifiedName();
        std::vector<OpenXmlQualifiedName> checkedAttributes;
        for (const auto& rule : Generated::PackageSchematronRelationshipRules())
        {
            if (rule.ContextElement != elementName)
            {
                continue;
            }

            checkedAttributes.push_back(rule.RelationshipAttribute);
            std::string_view relationshipId;
            if (!element.TryGetAttribute(rule.RelationshipAttribute, relationshipId) || relationshipId.empty())
            {
                continue;
            }

            const auto* relationship = FindRelationshipById(part, relationshipId);
            if (relationship == nullptr)
            {
                // The same identifier as the namespace-wide check below, so a
                // caller sees one kind of issue for one kind of defect whether
                // or not the SDK metadata happened to list the element.
                auto issue = OpenXmlPackageSemanticRulesHelper::MakeDanglingRelationshipReferenceIssue(
                    part, element, rule.RelationshipAttribute, relationshipId);
                if (!rule.RequiredRelationshipType.empty())
                {
                    issue.Message += " Expected relationship type '" + std::string(rule.RequiredRelationshipType) + "'.";
                }
                issue.ConstraintId = std::string(rule.TestExpression);
                sink.Report(std::move(issue));
                continue;
            }

            if (!rule.RequiredRelationshipType.empty() && relationship->Type != rule.RequiredRelationshipType)
            {
                sink.Report(MakePackageSchematronIssue("Schematron relationship rule failed: relationship has an unexpected type.",
                                                       part,
                                                       element,
                                                       *relationship,
                                                       rule.RequiredRelationshipType));
            }
        }

        OpenXmlPackageSemanticRulesHelper::ValidateRelationshipAttributes(part, element, checkedAttributes, sink);
    }

    static void CollectPackageSchematronUniqueValue(const OpenXmlPackagePart& part,
                                                    const OpenXMLElement& element,
                                                    std::span<const OpenXmlQualifiedName> elementPath,
                                                    const OpenXmlQualifiedName& attributeName,
                                                    bool caseInsensitive,
                                                    std::string_view testExpression,
                                                    std::unordered_set<std::string>& seenValues,
                                                    DiagnosticSink& sink)
    {
        if (MatchesElementPath(element, elementPath))
        {
            std::string_view value;
            if (element.TryGetAttribute(attributeName, value))
            {
                const auto normalizedValue = NormalizeSchematronUniqueValue(value, caseInsensitive);
                // Only membership matters: the duplicate is reported against the element
                // that repeats the value, not the one that introduced it. Keeping a
                // location for the first occurrence meant building one for every element
                // the rule looked at, which is the whole part.
                if (!seenValues.insert(normalizedValue).second)
                {
                    sink.Report(MakePackageSchematronElementIssue(
                        "Schematron unique-values rule failed: duplicate attribute value '" + std::string(value) + "'.",
                        part,
                        element,
                        testExpression));
                }
            }
        }
    }

    static void ValidatePackageSchematronUniqueValues(const OpenXmlPackagePart& part,
                                                      const std::shared_ptr<OpenXMLElement>& root,
                                                      DiagnosticSink& sink)
    {
        for (const auto& rule : Generated::PackageSchematronUniqueValueRules())
        {
            std::unordered_set<std::string> seenValues;
            VisitOpenXmlTree(root, [&](const OpenXMLElement& element)
                             { CollectPackageSchematronUniqueValue(part,
                                                                   element,
                                                                   rule.ElementPath,
                                                                   rule.Attribute,
                                                                   rule.CaseInsensitive,
                                                                   rule.TestExpression,
                                                                   seenValues,
                                                                   sink); });
        }
    }

    static void ValidatePackageSchematronAncestorUniqueValues(const OpenXmlPackagePart& part,
                                                              const std::shared_ptr<OpenXMLElement>& root,
                                                              DiagnosticSink& sink)
    {
        for (const auto& rule : Generated::PackageSchematronAncestorUniqueValueRules())
        {
            VisitOpenXmlTree(root, [&](const OpenXMLElement& ancestor)
                             {
                if (ancestor.QualifiedName() != rule.AncestorElement)
                {
                    return;
                }

                std::unordered_set<std::string> seenValues;
                for (const auto& child : ancestor.Children())
                {
                    VisitOpenXmlTree(child, [&](const OpenXMLElement& element) {
                        CollectPackageSchematronUniqueValue(part,
                                                            element,
                                                            rule.DescendantPath,
                                                            rule.Attribute,
                                                            rule.CaseInsensitive,
                                                            rule.TestExpression,
                                                            seenValues,
                                                            sink);
                    });
                } });
        }
    }

    static void ValidatePackageSchematronPartReferences(const OpenXmlPackage& package,
                                                        const OpenXmlPackagePart& part,
                                                        const std::shared_ptr<OpenXMLElement>& root,
                                                        DiagnosticSink& sink)
    {
        for (const auto& rule : Generated::PackageSchematronPartReferenceRules())
        {
            VisitOpenXmlTree(root, [&](const OpenXMLElement& element)
                             {
                if (element.QualifiedName() != rule.ContextElement)
                {
                    return;
                }

                std::string_view sourceValue;
                if (!element.TryGetAttribute(rule.SourceAttribute, sourceValue))
                {
                    return;
                }

                std::unordered_set<std::string> targetValues;
                const auto targetParts = ResolvePackageSchematronPartSelector(package, part, rule.PartSelector);
                for (const auto& targetPart : targetParts)
                {
                    if (targetPart)
                    {
                        CollectPackageSchematronAttributeValues(*targetPart,
                                                                rule.TargetElementPath,
                                                                rule.TargetAttribute,
                                                                targetValues);
                    }
                }

                if (targetValues.find(std::string(sourceValue)) == targetValues.end())
                {
                    sink.Report(MakePackageSchematronElementIssue(
                        "Schematron part-reference rule failed: referenced value '" + std::string(sourceValue)
                            + "' does not exist in target part selector '"
                            + std::string(rule.PartSelector.OriginalExpression) + "'.",
                        part,
                        element,
                        rule.TestExpression));
                } });
        }
    }

    static void ValidatePackageSchematronPartCounts(const OpenXmlPackage& package,
                                                    const OpenXmlPackagePart& part,
                                                    const std::shared_ptr<OpenXMLElement>& root,
                                                    DiagnosticSink& sink)
    {
        for (const auto& rule : Generated::PackageSchematronPartCountRules())
        {
            VisitOpenXmlTree(root, [&](const OpenXMLElement& element)
                             {
                if (element.QualifiedName() != rule.ContextElement)
                {
                    return;
                }

                std::string_view sourceValueText;
                if (!element.TryGetAttribute(rule.SourceAttribute, sourceValueText))
                {
                    return;
                }

                Real sourceValue = 0.0;
                if (!TryParseSchematronNumber(sourceValueText, sourceValue))
                {
                    sink.Report(MakePackageSchematronElementIssue(
                        "Schematron part-count rule failed: source attribute value '" + std::string(sourceValueText)
                            + "' is not numeric.",
                        part,
                        element,
                        rule.TestExpression));
                    return;
                }

                Size count = 0;
                const auto targetParts = ResolvePackageSchematronPartSelector(package, part, rule.PartSelector);
                for (const auto& targetPart : targetParts)
                {
                    if (targetPart)
                    {
                        count += CountPackageSchematronElements(*targetPart, rule.TargetElementPath);
                    }
                }

                const auto rhs = static_cast<Real>(count) + static_cast<Real>(rule.Offset);
                if (!EvaluateSchematronComparison(sourceValue, rule.ComparisonOperator, rhs))
                {
                    sink.Report(MakePackageSchematronElementIssue(
                        "Schematron part-count rule failed: source value '" + std::string(sourceValueText)
                            + "' does not satisfy comparison '"
                            + std::string(PackageSchematronComparisonOperatorText(rule.ComparisonOperator))
                            + "' against target count " + std::to_string(count) + " plus offset "
                            + std::to_string(rule.Offset) + ".",
                        part,
                        element,
                        rule.TestExpression));
                } });
        }
    }

    static void ValidatePackageSchematronTree(const OpenXmlPackagePart& part,
                                              const std::shared_ptr<OpenXMLElement>& element,
                                              DiagnosticSink& sink)
    {
        VisitOpenXmlTree(element, [&](const OpenXMLElement& current)
                         { ValidatePackageSchematronElement(part, current, sink); });
    }

    static void ValidateContainerRelationships(const OpenXmlPackage& package,
                                               const OpenXmlPartContainer& container,
                                               std::string_view sourceUri,
                                               DiagnosticSink& sink)
    {
        std::unordered_set<std::string> relationshipIds;
        for (const auto& relationship : container.Relationships())
        {
            if (relationship.Id.empty())
            {
                sink.Report(MakeOpcIssue(ValidationErrorId::OpcEmptyRelationshipId,
                                         "OPC relationship is missing an Id.",
                                         sourceUri,
                                         relationship));
            }
            else if (!relationshipIds.insert(relationship.Id).second)
            {
                sink.Report(MakeOpcIssue(ValidationErrorId::OpcDuplicateRelationshipId,
                                         "OPC relationship Id is duplicated within one source container.",
                                         sourceUri,
                                         relationship));
            }

            if (relationship.Type.empty())
            {
                sink.Report(MakeOpcIssue(ValidationErrorId::OpcEmptyRelationshipType,
                                         "OPC relationship is missing a Type.",
                                         sourceUri,
                                         relationship));
            }

            if (relationship.Target.empty())
            {
                sink.Report(MakeOpcIssue(ValidationErrorId::OpcEmptyRelationshipTarget,
                                         "OPC relationship is missing a Target.",
                                         sourceUri,
                                         relationship));
                continue;
            }

            if (!relationship.TargetMode.empty() && relationship.TargetMode != "External")
            {
                sink.Report(MakeOpcIssue(ValidationErrorId::OpcInvalidRelationshipTargetMode,
                                         "OPC relationship TargetMode must be External when present.",
                                         sourceUri,
                                         relationship));
            }

            if (relationship.IsExternal)
            {
                continue;
            }

            const auto resolvedTarget = Detail::ResolveRelationshipTarget(sourceUri, relationship.Target);
            if (package.GetPartByUri(resolvedTarget) == nullptr)
            {
                auto issue = MakeOpcIssue(ValidationErrorId::OpcDanglingRelationshipTarget,
                                          "OPC relationship target does not resolve to a package part.",
                                          sourceUri,
                                          relationship);
                issue.TargetUri = resolvedTarget;
                sink.Report(std::move(issue));
            }
        }
    }

    static ValidationIssue MakePackageIssue(ValidationErrorId id, std::string message, std::string_view partUri = {})
    {
        ValidationIssue issue;
        issue.Severity = ValidationSeverity::Error;
        issue.Domain = ValidationDomain::Packaging;
        issue.Id = id;
        issue.Message = std::move(message);
        issue.PartUri = std::string(partUri);
        return issue;
    }

    static bool HasValidDescriptorContentType(const OpenXmlPackagePart& part) noexcept
    {
        const auto name = part.Descriptor().Name;
        const auto contentType = part.ContentType();
        if (contentType == part.Descriptor().ContentType)
        {
            return true;
        }
        if (name == "MainDocumentPart")
        {
            return contentType == "application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml" || contentType == "application/vnd.openxmlformats-officedocument.wordprocessingml.template.main+xml" || contentType == "application/vnd.ms-word.document.macroEnabled.main+xml" || contentType == "application/vnd.ms-word.template.macroEnabledTemplate.main+xml";
        }
        if (name == "WorkbookPart")
        {
            return contentType == "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml" || contentType == "application/vnd.openxmlformats-officedocument.spreadsheetml.template.main+xml" || contentType == "application/vnd.ms-excel.sheet.macroEnabled.main+xml" || contentType == "application/vnd.ms-excel.template.macroEnabled.main+xml";
        }
        if (name == "PresentationPart")
        {
            return contentType == "application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml" || contentType == "application/vnd.ms-powerpoint.presentation.macroEnabled.main+xml" || contentType == "application/vnd.openxmlformats-officedocument.presentationml.template.main+xml" || contentType == "application/vnd.ms-powerpoint.template.macroEnabled.main+xml" || contentType == "application/vnd.openxmlformats-officedocument.presentationml.slideshow.main+xml" || contentType == "application/vnd.ms-powerpoint.slideshow.macroEnabled.main+xml";
        }
        return false;
    }

    static void ValidatePartSemantics(const OpenXmlPackage& package,
                                      const OpenXmlPackagePart& part,
                                      OpenXml::FileFormatVersions targetVersion,
                                      DiagnosticSink& sink)
    {
        if (!SupportsOfficeVersion(targetVersion, part.Descriptor().Version))
        {
            sink.Report(MakePackageIssue(ValidationErrorId::PartVersionViolation,
                                         "Package part is not available in the requested Office version.",
                                         part.Uri()));
        }

        const auto partName = part.Descriptor().Name;
        const bool isMainPart = partName == "MainDocumentPart" || partName == "WorkbookPart" || partName == "PresentationPart";
        if ((isMainPart || !part.Descriptor().ContentType.empty()) && !HasValidDescriptorContentType(part))
        {
            auto issue = MakePackageIssue(ValidationErrorId::PackageContentTypeMismatch,
                                          "Package part content type does not match its descriptor.",
                                          part.Uri());
            issue.TargetUri = std::string(part.Uri());
            sink.Report(std::move(issue));
        }

        // A part may be the target of more than one relationship, and the extra
        // ones do not have to carry its own type. PowerPoint relates an
        // embedded media part twice - once as `.../audio` and once as
        // `.../office/2007/relationships/media` - and reading the second as a
        // mismatch reported an error on files PowerPoint itself wrote. What is
        // worth reporting is a part reached only by a relationship of the wrong
        // type, so the descriptor-typed edges are collected first.
        std::unordered_set<std::string> properlyTyped;
        for (const auto& relationship : part.Relationships())
        {
            if (relationship.IsExternal || relationship.Target.empty())
            {
                continue;
            }
            const auto targetUri = Detail::ResolveRelationshipTarget(part.Uri(), relationship.Target);
            const auto target = package.GetPartByUri(targetUri);
            if (target && relationship.Type == target->Descriptor().RelationshipType)
            {
                properlyTyped.insert(targetUri);
            }
        }

        for (const auto& relationship : part.Relationships())
        {
            if (relationship.IsExternal || relationship.Target.empty())
            {
                continue;
            }
            const auto targetUri = Detail::ResolveRelationshipTarget(part.Uri(), relationship.Target);
            const auto target = package.GetPartByUri(targetUri);
            if (target && !target->Descriptor().RelationshipType.empty() &&
                relationship.Type != target->Descriptor().RelationshipType &&
                !properlyTyped.contains(targetUri))
            {
                auto issue = MakePackageIssue(ValidationErrorId::PackageRelationshipTypeMismatch,
                                              "Relationship type does not match the target part descriptor.",
                                              part.Uri());
                issue.RelationshipSourceUri = part.Uri();
                issue.RelationshipId = relationship.Id;
                issue.RelationshipType = relationship.Type;
                issue.TargetUri = targetUri;
                sink.Report(std::move(issue));
            }
        }
    }

    static void ValidateMainPart(const OpenXmlPackage& package, DiagnosticSink& sink)
    {
        std::string_view expectedName;
        if (dynamic_cast<const Packaging::WordprocessingDocument*>(&package))
        {
            expectedName = "MainDocumentPart";
        }
        else if (dynamic_cast<const Packaging::SpreadsheetDocument*>(&package))
        {
            expectedName = "WorkbookPart";
        }
        else if (dynamic_cast<const Packaging::PresentationDocument*>(&package))
        {
            expectedName = "PresentationPart";
        }
        else
        {
            return;
        }

        Size count = 0;
        for (const auto& part : package.Parts())
        {
            if (part && part->Descriptor().Name == expectedName)
            {
                ++count;
            }
        }
        if (count == 0)
        {
            sink.Report(MakePackageIssue(ValidationErrorId::PackageMissingMainPart,
                                         "Package is missing its required " + std::string(expectedName) + "."));
        }
        else if (count > 1)
        {
            sink.Report(MakePackageIssue(ValidationErrorId::PackageMultipleMainParts,
                                         "Package contains more than one " + std::string(expectedName) + "."));
        }
    }

    /**
     * @brief Checks that an XML part really is one well-formed XML document.
     *
     * A part that cannot be parsed at all never reaches this point - the package
     * loader rejects it - but pugixml, like most pull parsers used for OPC, accepts
     * a buffer carrying several document elements. Such a part loads, validates
     * against the schema (the first root is checked) and saves again, while Word
     * and every conforming XML parser reject the file outright. The rule is checked
     * explicitly here so the tool cannot pronounce a package clean that no other
     * application will open.
     *
     * The part is re-serialized and re-parsed rather than inspected in place; the
     * validator has no access to the part's internal document, and validation is
     * not a hot path.
     */
    static void ValidateXmlPartWellFormed(const OpenXmlPackagePart& part, DiagnosticSink& sink)
    {
        const auto xml = part.GetXmlString();

        Pugi::xml_document document;
        const auto parsed = document.load_buffer(xml.data(), xml.size(), Xml::ParseOptions::Preserving);

        Size documentElements = 0;
        for (auto child = document.first_child(); child; child = child.next_sibling())
        {
            if (child.type() == Pugi::node_element)
            {
                ++documentElements;
            }
        }

        if (parsed && documentElements == 1)
        {
            return;
        }

        ValidationIssue issue;
        issue.Severity = ValidationSeverity::Error;
        issue.Domain = ValidationDomain::Opc;
        issue.Id = ValidationErrorId::OpcMalformedPartXml;
        issue.PartUri = part.Uri();
        if (!parsed)
        {
            issue.Message = std::string("XML part is not well formed: ") + parsed.description() + ".";
        }
        else if (documentElements == 0)
        {
            issue.Message = "XML part is not well formed: it has no root element.";
        }
        else
        {
            issue.Message = "XML part is not well formed: it has " + std::to_string(documentElements) +
                            " root elements, but an XML document must have exactly one.";
        }
        sink.Report(std::move(issue));
    }

    static void ValidatePartTree(const OpenXmlPackage& package,
                                 const OpenXmlPackagePart& part,
                                 DiagnosticSink& sink,
                                 std::unordered_set<const OpenXmlPackagePart*>& visited,
                                 const OpenXmlDomValidationSettings& domSettings,
                                 bool validateDom)
    {
        if (!visited.insert(&part).second)
        {
            return;
        }
        ValidateContainerRelationships(package, part, part.Uri(), sink);
        ValidatePartSemantics(package, part, domSettings.TargetVersion, sink);
        if (part.IsXmlPart())
        {
            ValidateXmlPartWellFormed(part, sink);
            auto root = part.GetRootElement();
            if (root && validateDom)
            {
                PartDiagnosticSink partSink(sink, part.Uri());
                OpenXmlDomValidator(domSettings).Validate(*root, partSink);
            }
            ValidatePackageSchematronTree(part, root, sink);
            ValidatePackageSchematronUniqueValues(part, root, sink);
            ValidatePackageSchematronAncestorUniqueValues(part, root, sink);
            ValidatePackageSchematronPartReferences(package, part, root, sink);
            ValidatePackageSchematronPartCounts(package, part, root, sink);
            OpenXmlPackageSemanticRulesHelper::ValidatePart(package, part, root, sink);
        }
        for (const auto& child : part.Parts())
        {
            if (child)
            {
                ValidatePartTree(package, *child, sink, visited, domSettings, validateDom);
            }
        }
    }

    /**
     * @brief Rejects a package that declares its main part the ISO 29500 Strict way.
     *
     * Strict is a different conformance class, not a dialect: every markup
     * namespace and relationship type is spelled under `purl.oclc.org`, so nothing
     * in the generated DOM - which is built from the Transitional schemas - matches
     * such a document. Validation says so rather than reporting a structurally fine
     * package, because "no errors" would otherwise mean "this file is supported" to
     * every caller, which is exactly what it is not.
     *
     * The OPC container itself is class-independent, so the package still loads and
     * the package-level tools still work on it; see docs/Compatibility.md.
     */
    static void ValidateConformanceClass(const OpenXmlPackage& package, DiagnosticSink& sink)
    {
        for (const auto& relationship : package.Relationships())
        {
            if (relationship.IsExternal || !ConformanceClass::IsStrictOfficeDocument(relationship.Type))
            {
                continue;
            }

            ValidationIssue issue;
            issue.Severity = ValidationSeverity::Error;
            issue.Domain = ValidationDomain::Packaging;
            issue.Id = ValidationErrorId::PackageStrictConformanceUnsupported;
            issue.Message =
                "Package uses the ISO 29500 Strict conformance class, which this library does not "
                "implement; only Transitional is supported. Re-save the document as Transitional.";
            issue.RelationshipSourceUri = "/";
            issue.RelationshipId = relationship.Id;
            issue.RelationshipType = relationship.Type;
            issue.TargetUri = relationship.Target;
            sink.Report(std::move(issue));
            return;
        }
    }

    static void ValidateDuplicatePartUris(const std::vector<std::string>& duplicatePartUris, DiagnosticSink& sink)
    {
        std::unordered_set<std::string> reportedUris;
        for (const auto& uri : duplicatePartUris)
        {
            if (!reportedUris.insert(uri).second)
            {
                continue;
            }

            ValidationIssue issue;
            issue.Severity = ValidationSeverity::Error;
            issue.Domain = ValidationDomain::Opc;
            issue.Id = ValidationErrorId::OpcDuplicatePartUri;
            issue.Message = "OPC package contains multiple ZIP entries that resolve to the same part URI.";
            issue.PartUri = uri;
            issue.TargetUri = uri;
            sink.Report(std::move(issue));
        }
    }
};

OpenXmlPackageValidator::OpenXmlPackageValidator(OpenXmlDomValidationSettings domSettings) noexcept
    : domSettings_(domSettings), validateDom_(true)
{
}

ValidationResult OpenXmlPackageValidator::Validate(const OpenXmlPackage& package) const
{
    ValidationResult result;
    Validate(package, result);
    return result;
}

void OpenXmlPackageValidator::Validate(const OpenXmlPackage& package, DiagnosticSink& sink) const
{
    OpenXmlPackageValidatorHelper::ValidateDuplicatePartUris(package.m_impl->duplicatePartUris, sink);
    OpenXmlPackageValidatorHelper::ValidateConformanceClass(package, sink);
    OpenXmlPackageValidatorHelper::ValidateContainerRelationships(package, package, "/", sink);
    OpenXmlPackageValidatorHelper::ValidateMainPart(package, sink);
    std::unordered_set<const OpenXmlPackagePart*> visited;
    for (const auto& part : package.Parts())
    {
        if (part)
        {
            OpenXmlPackageValidatorHelper::ValidatePartTree(package, *part, sink, visited, domSettings_, validateDom_);
        }
    }
    // Preserve/opaque or malformed packages may contain parts that are not
    // reachable from a package relationship. They still need XML/schema and
    // local relationship validation, so finish by walking the URI registry.
    for (const auto& entry : package.m_impl->partsByUri)
    {
        const auto& part = entry.second;
        if (part)
        {
            OpenXmlPackageValidatorHelper::ValidatePartTree(package, *part, sink, visited, domSettings_, validateDom_);
        }
    }
}

} // namespace ExyokiOffice
