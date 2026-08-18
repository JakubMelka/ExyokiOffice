// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

// Shared scaffolding of the DOM-validation unit suites: wraps one XML part in a
// package, so the validator sees a live tree with real namespace declarations
// and parent links, and reduces a validation result to its error messages.
#pragma once

#include "doctest.h"

#include "ExyokiOffice/OpenXMLElement.hpp"
#include "ExyokiOffice/OpenXmlDomValidator.hpp"
#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/OpenXmlPackagePart.hpp"
#include "ExyokiOffice/StandardTypes.hpp"
#include "ExyokiOffice/ValidationResult.hpp"
#include "zip/zip.h"

#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace ExyokiOfficeTests::DomValidation
{

inline void AddZipEntry(zip_t* archive, const char* name, std::string_view content)
{
    REQUIRE(zip_entry_open(archive, name) == 0);
    CHECK(zip_entry_write(archive, content.data(), content.size()) == 0);
    zip_entry_close(archive);
}

/**
 * @brief Wraps one XML part in a package so the DOM can be loaded from it.
 *
 * The validator works on a live element tree, and only a package produces one
 * whose namespace declarations and parent links are those of a real document -
 * which `mc:Ignorable` needs, because resolving its prefixes depends on both.
 */
inline std::vector<ExyokiOffice::Byte> BuildSingleXmlPartPackage(std::string_view xml)
{
    auto* archive = zip_stream_open(nullptr, 0, ZIP_DEFAULT_COMPRESSION_LEVEL, 'w');
    REQUIRE(archive != nullptr);

    AddZipEntry(archive,
                "[Content_Types].xml",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="xml" ContentType="application/xml"/>
</Types>)");
    AddZipEntry(archive, "custom.xml", xml);

    void* rawBuffer = nullptr;
    ExyokiOffice::Size rawSize = 0;
    REQUIRE(zip_stream_copy(archive, &rawBuffer, &rawSize) > 0);
    zip_stream_close(archive);
    REQUIRE(rawBuffer != nullptr);

    const auto* bytes = static_cast<const ExyokiOffice::UInt8*>(rawBuffer);
    std::vector<ExyokiOffice::Byte> result(bytes, bytes + rawSize);
    std::free(rawBuffer);
    return result;
}

/**
 * @brief Loads @p xml and returns the errors a full-tree DOM validation reports,
 * joined into one string so a failing CHECK shows them.
 */
inline std::string ValidationErrors(std::string_view xml)
{
    // The package owns the DOM, and the returned strings are read after it goes
    // out of scope, so it is kept alive only for the duration of the walk.
    ExyokiOffice::OpenXmlPackage package;
    REQUIRE(package.LoadFromMemory(BuildSingleXmlPartPackage(xml)));

    auto part = package.GetPartByUri("/custom.xml");
    REQUIRE(part != nullptr);
    auto root = part->GetRootElement();
    REQUIRE(root != nullptr);

    const auto result = ExyokiOffice::OpenXmlDomValidator().Validate(*root);

    std::string errors;
    for (const auto& issue : result.Issues())
    {
        if (issue.Severity == ExyokiOffice::ValidationSeverity::Error)
        {
            if (!errors.empty())
            {
                errors += " | ";
            }
            errors += issue.Message + " at " + issue.Location.Path;
        }
    }
    return errors;
}

} // namespace ExyokiOfficeTests::DomValidation

