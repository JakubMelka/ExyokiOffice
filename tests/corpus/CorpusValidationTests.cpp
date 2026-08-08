// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

// Validation over the file corpus.
//
// Everything under corpus/ was written by a released Microsoft Office build, so
// a diagnostic raised against one of these files is a statement about the
// validator, not about the document. That makes this the layer that keeps the
// schema and schematron rules honest: a rule that misreads its own metadata
// shows up here as a document Office produced and the library rejects.

#include "doctest.h"

#include "CorpusManifest.hpp"

#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/OpenXmlPackageValidator.hpp"
#include "ExyokiOffice/ValidationResult.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using ExyokiOfficeTests::CorpusDocument;
using ExyokiOfficeTests::CorpusManifest;
using ExyokiOfficeTests::CorpusShard;

/** One rendered diagnostic, in the form CAPTURE can show. */
std::string Describe(const ExyokiOffice::ValidationIssue& issue)
{
    std::string text = issue.Message;
    if (!issue.ConstraintId.empty())
    {
        text += " [" + issue.ConstraintId + "]";
    }
    if (!issue.PartUri.empty())
    {
        text += " in " + issue.PartUri;
    }
    if (!issue.Location.Path.empty())
    {
        text += " at " + issue.Location.Path;
    }
    return text;
}

/** Errors and warnings a full package validation raised, already rendered. */
struct ValidationDiagnostics
{
    std::vector<std::string> Errors;
    std::vector<std::string> Warnings;
};

ValidationDiagnostics ValidateForVersion(const CorpusDocument& document,
                                         ExyokiOffice::OpenXml::FileFormatVersions version)
{
    ValidationDiagnostics diagnostics;

    ExyokiOffice::OpenXmlPackage package;
    if (!package.LoadFromFile(document.Path()))
    {
        diagnostics.Errors.emplace_back("package did not load");
        return diagnostics;
    }

    // The settings constructor is the one that turns on per-part DOM and schema
    // validation; the default constructor checks only OPC structure, which
    // would make every case here pass without looking at any markup.
    const ExyokiOffice::OpenXmlPackageValidator validator(
        ExyokiOffice::OpenXmlDomValidationSettings{version});

    // The result has to outlive the loop: Issues() hands out a reference into
    // it, and iterating over a temporary's member reads freed memory.
    const auto result = validator.Validate(package);
    for (const auto& issue : result.Issues())
    {
        auto& bucket = issue.Severity == ExyokiOffice::ValidationSeverity::Error ? diagnostics.Errors
                                                                                 : diagnostics.Warnings;
        bucket.push_back(Describe(issue));
    }
    return diagnostics;
}

/** Joins rendered diagnostics so a failing CHECK prints them. */
std::string Join(const std::vector<std::string>& lines)
{
    std::string joined;
    for (const auto& line : lines)
    {
        if (!joined.empty())
        {
            joined += " | ";
        }
        joined += line;
    }
    return joined;
}

/** Looks up one manifest entry by its `File` value. */
const CorpusDocument& FindDocument(std::string_view file)
{
    const auto& documents = CorpusManifest();
    const auto found = std::find_if(documents.begin(), documents.end(),
                                    [file](const CorpusDocument& candidate)
                                    { return candidate.File == file; });
    REQUIRE(found != documents.end());
    return *found;
}
} // namespace

TEST_SUITE("Corpus validation")
{

    TEST_CASE("every corpus document validates cleanly [corpus] [corpus-validation]")
    {
        // Errors and warnings in one case on purpose: a full package validation
        // of the corpus is the most expensive thing this suite does, and running
        // it twice to split the two severities would double that for nothing.
        //
        // CorpusShard rather than CorpusManifest: this case is registered as one
        // CTest entry per corpus document, so the sweep is over the one document
        // this process was given.
        for (const auto& document : CorpusShard())
        {
            CAPTURE(document.File);
            const auto diagnostics =
                ValidateForVersion(document, ExyokiOffice::OpenXml::FileFormatVersions::Microsoft365);
            CHECK(Join(diagnostics.Errors) == "");
            CHECK(Join(diagnostics.Warnings) == "");
        }
    }

    TEST_CASE("an older target generation rejects the markup it predates [corpus] [corpus-version-target]")
    {
        // The fixtures were saved by Office 2016 or later, so they use markup no
        // Office 2007 consumer could read. Validating them against that target
        // has to say so: a TargetVersion that quietly stopped applying, or a
        // validator constructed so that it never looks at the markup, would let
        // the clean run above pass for the wrong reason.
        //
        // One document per family rather than the whole corpus. The claim is
        // about the version rules, which are family-independent, and every extra
        // document costs another full validation. That is also why the case has
        // a tag of its own instead of sharing [corpus-validation]: the areas
        // carrying that tag are registered once per corpus document, and a case
        // that names its own fixtures would be run in each of those entries.
        for (const char* file : {"word/Language_Learning_Across_Countries_and_CEFR_Proficiency_Levels.docx",
                                 "excel/Dashboard.xlsx",
                                 "powerpoint/Architecture_of_Modern_LLM_Systems.pptx"})
        {
            CAPTURE(file);
            const auto& document = FindDocument(file);
            const auto diagnostics =
                ValidateForVersion(document, ExyokiOffice::OpenXml::FileFormatVersions::Office2007);
            CHECK_FALSE(diagnostics.Errors.empty());
        }
    }

    TEST_CASE("the two content-model matchers agree on every corpus document [corpus] [corpus-content-model]")
    {
        // Content models are matched by an automaton compiled from the schema
        // particle tree. `CrossCheckContentModel` runs the recursive matcher it
        // replaced alongside it and reports a diagnostic wherever the two
        // disagree, which is what this case looks for.
        //
        // The unit tests put the same question to randomly generated models,
        // where a counterexample is small enough to read. This asks it of the
        // models Office actually writes against documents Office actually
        // saved - many of which no generator would think to produce - and it is
        // the reason the recursive matcher is still in the build.
        //
        // It is an area of its own because it pays for both matchers: the
        // recursive one is what made a 200-paragraph body take minutes, and
        // running it over the whole corpus is by a wide margin the slowest thing
        // the suite does. Like the validation sweep above it is registered as
        // one CTest entry per document, so those payments run concurrently.
        ExyokiOffice::OpenXmlDomValidationSettings settings{
            ExyokiOffice::OpenXml::FileFormatVersions::Microsoft365};
        settings.CrossCheckContentModel = true;
        const ExyokiOffice::OpenXmlPackageValidator validator(settings);

        for (const auto& document : CorpusShard())
        {
            CAPTURE(document.File);
            ExyokiOffice::OpenXmlPackage package;
            REQUIRE(package.LoadFromFile(document.Path()));

            const auto result = validator.Validate(package);
            std::vector<std::string> mismatches;
            for (const auto& issue : result.Issues())
            {
                if (issue.Id == ExyokiOffice::ValidationErrorId::ContentModelCrossCheckMismatch)
                {
                    mismatches.push_back(Describe(issue));
                }
            }
            CHECK(Join(mismatches) == "");
        }
    }
}
