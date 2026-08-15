// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/FileFormatVersions.h"
#include "ExyokiOffice/ValidationResult.hpp"

namespace ExyokiOffice
{

class OpenXMLElement;

struct OpenXmlDomValidationSettings
{
    /** Office generation whose schema availability rules should be applied. */
    OpenXml::FileFormatVersions TargetVersion{OpenXml::FileFormatVersions::Microsoft365};

    /**
     * @brief Checks every content-model verdict against the reference matcher.
     *
     * Content models are matched by an automaton compiled from the schema
     * particle tree, which is fast but is also the kind of code whose wrong
     * answers look exactly like right ones. The library keeps the slow recursive
     * matcher the automaton replaced, and turning this on runs both on every
     * element and reports a @ref ValidationErrorId::ContentModelCrossCheckMismatch
     * wherever they disagree - which is a defect in this library rather than in
     * the document, and is worth reporting as one.
     *
     * Off by default: it makes validation slower than it was before the
     * automaton existed, because it pays for both. Turn it on to investigate a
     * content-model verdict that looks wrong, or to sweep a corpus for
     * disagreements.
     */
    bool CrossCheckContentModel{false};
};

/** Validates generated schema metadata for an Open XML element tree. */
class EXYOKIOFFICE_EXPORT OpenXmlDomValidator
{
public:
    explicit OpenXmlDomValidator(OpenXmlDomValidationSettings settings = {}) noexcept;

    /** Validates `element` and every descendant. */
    [[nodiscard]] ValidationResult Validate(const OpenXMLElement& element) const;
    void Validate(const OpenXMLElement& element, DiagnosticSink& sink) const;

    /** Validates only constraints and the content model of `element`. */
    [[nodiscard]] ValidationResult ValidateElement(const OpenXMLElement& element) const;
    void ValidateElement(const OpenXMLElement& element, DiagnosticSink& sink) const;

private:
    OpenXmlDomValidationSettings settings_;
};

} // namespace ExyokiOffice
