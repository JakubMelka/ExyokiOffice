// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/OpenXmlPackagePart.hpp"

#include "pugixml/pugixml.hpp"

#include <optional>
#include <string>
#include <vector>

namespace ExyokiOffice::Security
{

/// Selection criteria taken from the Transform element of a signature.
struct RelationshipSelection
{
    /// Relationship identifiers listed as mdssi:RelationshipReference/@SourceId.
    std::vector<std::string> SourceIds;
    /// Relationship types listed as mdssi:RelationshipsGroupReference/@SourceType.
    std::vector<std::string> SourceTypes;
};

/// \brief The OPC RelationshipTransform (ECMA-376 Part 2).
///
/// A relationship part is never hashed as stored. A signature first selects the
/// relationships it cares about (by identifier or by type), drops everything
/// else, sorts what remains by identifier, and makes the TargetMode attribute
/// explicit. Only that normalized XML is canonicalized and digested. The point
/// is that adding an unrelated relationship later does not break the signature.
///
/// The transform input is rebuilt from the in-memory relationship model rather
/// than from the stored bytes, because the model carries exactly the four
/// attributes the transform keeps.
class EXYOKIOFFICE_EXPORT RelationshipTransform final
{
public:
    RelationshipTransform() = delete;

    /// Reads the selection out of the mdssi children of a Transform element.
    static RelationshipSelection ReadSelection(const Pugi::xml_node& transformNode);

    /// Builds the transform output for the relationships of \p container.
    /// An empty selection keeps every relationship, as the specification requires.
    /// @return Nothing when the transform output could not be canonicalized.
    static std::optional<std::string> Apply(const OpenXmlPartContainer& container,
                                            const RelationshipSelection& selection);

    /// Builds the transform output for an explicit relationship list.
    /// @return Nothing when the transform output could not be canonicalized.
    static std::optional<std::string> Apply(const std::vector<OpenXmlRelationship>& relationships,
                                            const RelationshipSelection& selection);
};

} // namespace ExyokiOffice::Security
