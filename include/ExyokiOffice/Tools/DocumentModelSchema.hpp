// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/Tools/PackageModel.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace ExyokiOffice::Tools
{

/**
 * @brief JSON Schema (draft 07) for the `exyokioffice-document` envelope.
 *
 * The schema describes exactly what SerializeModelJson writes and what
 * ParseModelJson expects from a well-formed importer: the envelope header,
 * core properties, media references, and the family-specific `document`
 * payload. It is the machine-readable counterpart of
 * docs/tools/conversion-formats.md.
 *
 * The schema and the serializer are kept in step by tests rather than by
 * generation: every object closes with `additionalProperties: false`, so a
 * field added to the serializer without a matching schema entry fails the
 * conformance suite in tests/tools immediately.
 *
 * Two deliberate asymmetries with the serializer, both in the importer's
 * favour: fields the serializer omits at their default value stay optional,
 * and booleans accept `false` even though the serializer never writes it.
 * Everything the serializer can emit validates; a document that validates is
 * accepted by the parser.
 *
 * @return The schema as pretty-printed JSON, terminated by a newline.
 */
[[nodiscard]] EXYOKIOFFICE_EXPORT std::string GetDocumentModelJsonSchema();

/// File name under which the schema is published (docs/schemas/).
[[nodiscard]] EXYOKIOFFICE_EXPORT std::string GetDocumentModelJsonSchemaFileName();

/**
 * @brief Validates one JSON envelope against GetDocumentModelJsonSchema().
 *
 * Every violation is reported as an Error diagnostic whose context is the
 * JSON pointer of the offending value; unparsable input is a single Error.
 * Validation is purely structural — cross-references (a `commentRef` naming a
 * missing comment, an image naming a missing media entry) are the parser's
 * business and are not checked here.
 *
 * @return True when the envelope conforms to the schema.
 */
[[nodiscard]] EXYOKIOFFICE_EXPORT bool ValidateModelJson(std::string_view json,
                                                         std::vector<ToolDiagnostic>& diagnostics);

} // namespace ExyokiOffice::Tools
