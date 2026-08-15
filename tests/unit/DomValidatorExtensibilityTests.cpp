// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

// ---------------------------------------------------------------------------
// Regression suite for the three ways Open XML content models are deliberately
// open-ended, and which schema validation therefore may not treat as errors.
//
//  - ECMA-376 Part 3 lets `mc:AlternateContent` stand wherever an element of
//    the surrounding vocabulary may stand. It holds several renderings of that
//    one element and a consumer takes the branch it understands, so the model
//    cannot say which name is there.
//  - the same part lets a producer declare whole namespaces `mc:Ignorable`.
//    A consumer that does not understand one drops its elements and carries on,
//    so they cannot invalidate the content around them.
//  - every vocabulary carries the `extLst`/`ext` extension idiom, whose payload
//    the schema declares as `xsd:any processContents="lax"`.
//
// Each of these was a source of spurious errors against packages Microsoft
// Office wrote; the corpus layer measures that end to end, and these cases pin
// the individual rules on markup small enough to read.
// ---------------------------------------------------------------------------

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

namespace DomValidatorExtensibilityHelpers
{

void AddZipEntry(zip_t* archive, const char* name, std::string_view content)
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
std::vector<ExyokiOffice::Byte> BuildSingleXmlPartPackage(std::string_view xml)
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
std::string ValidationErrors(std::string_view xml)
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

} // namespace DomValidatorExtensibilityHelpers

TEST_SUITE("DOM validation of open-ended content models")
{
    using DomValidatorExtensibilityHelpers::ValidationErrors;

    TEST_CASE("mc:AlternateContent stands in for the element it wraps [unit] [dom-extensibility]")
    {
        // What Excel writes into every chart: the 2010 style value in a choice,
        // the original c:style as the fallback. Neither branch name may be
        // demanded at that position, because which one is read is the
        // consumer's decision, not the schema's.
        const auto errors = ValidationErrors(
            R"(<?xml version="1.0" encoding="UTF-8"?>
<c:chartSpace xmlns:c="http://schemas.openxmlformats.org/drawingml/2006/chart"
              xmlns:c14="http://schemas.microsoft.com/office/drawing/2007/8/2/chart"
              xmlns:mc="http://schemas.openxmlformats.org/markup-compatibility/2006">
  <c:date1904 val="0"/>
  <mc:AlternateContent>
    <mc:Choice Requires="c14"><c14:style val="102"/></mc:Choice>
    <mc:Fallback><c:style val="2"/></mc:Fallback>
  </mc:AlternateContent>
</c:chartSpace>)");

        CHECK(errors == "");
    }

    TEST_CASE("an ignorable namespace does not break the content model [unit] [dom-extensibility]")
    {
        // `c:date1904` and `c:roundedCorners` are consecutive in CT_ChartSpace.
        // The xr element between them belongs to a namespace the root declares
        // ignorable, so a consumer that does not know it drops it and still
        // sees a valid chart space.
        const auto errors = ValidationErrors(
            R"(<?xml version="1.0" encoding="UTF-8"?>
<c:chartSpace xmlns:c="http://schemas.openxmlformats.org/drawingml/2006/chart"
              xmlns:mc="http://schemas.openxmlformats.org/markup-compatibility/2006"
              xmlns:xr="http://schemas.microsoft.com/office/spreadsheetml/2014/revision"
              mc:Ignorable="xr">
  <c:date1904 val="0"/>
  <xr:revisionPtr revIDLastSave="0" documentId="8_{F836CEFC-F02A-4900-B730-FBB43223F871}"/>
  <c:roundedCorners val="0"/>
</c:chartSpace>)");

        CHECK(errors == "");
    }

    TEST_CASE("an unignorable stray element is still an error [unit] [dom-extensibility]")
    {
        // The same document without the mc:Ignorable declaration. Nothing tells
        // a consumer it may drop the element, so the content model applies and
        // the validator has to say the child does not belong there.
        const auto errors = ValidationErrors(
            R"(<?xml version="1.0" encoding="UTF-8"?>
<c:chartSpace xmlns:c="http://schemas.openxmlformats.org/drawingml/2006/chart"
              xmlns:xr="http://schemas.microsoft.com/office/spreadsheetml/2014/revision">
  <c:date1904 val="0"/>
  <xr:revisionPtr revIDLastSave="0" documentId="8_{F836CEFC-F02A-4900-B730-FBB43223F871}"/>
  <c:roundedCorners val="0"/>
</c:chartSpace>)");

        CHECK_FALSE(errors.empty());
    }

    TEST_CASE("an ignorable namespace does not excuse markup the model declares [unit] [dom-extensibility]")
    {
        // `c` itself is declared ignorable here, which must not turn the chart
        // vocabulary into markup the validator skips: an element the content
        // model names is one the consumer understands, so it is checked in
        // place. `c:roundedCorners` precedes `c:date1904` in CT_ChartSpace.
        const auto errors = ValidationErrors(
            R"(<?xml version="1.0" encoding="UTF-8"?>
<c:chartSpace xmlns:c="http://schemas.openxmlformats.org/drawingml/2006/chart"
              xmlns:mc="http://schemas.openxmlformats.org/markup-compatibility/2006"
              mc:Ignorable="c">
  <c:roundedCorners val="0"/>
  <c:date1904 val="0"/>
</c:chartSpace>)");

        CHECK_FALSE(errors.empty());
    }

    TEST_CASE("an extension payload is not checked against a modelled extension [unit] [dom-extensibility]")
    {
        // The content model of c:extLst names one specific extension type, but
        // a real extLst holds whichever extensions the producer emitted, told
        // apart by @uri. Two of Excel's, neither of them the modelled one.
        const auto errors = ValidationErrors(
            R"(<?xml version="1.0" encoding="UTF-8"?>
<c:chartSpace xmlns:c="http://schemas.openxmlformats.org/drawingml/2006/chart"
              xmlns:c16r3="http://schemas.microsoft.com/office/drawing/2017/03/chart"
              xmlns:c15="http://schemas.microsoft.com/office/drawing/2012/chart">
  <c:chart>
    <c:plotArea><c:layout/></c:plotArea>
    <c:extLst>
      <c:ext uri="{56B9EC1D-385E-4148-901F-78D8002777C0}">
        <c16r3:dataDisplayOptions16><c16r3:dispNaAsBlank val="1"/></c16r3:dataDisplayOptions16>
      </c:ext>
      <c:ext uri="{CE6537A1-D6FC-4f65-9D91-7224C49458BB}">
        <c15:showLeaderLines val="1"/>
        <c15:leaderLines/>
      </c:ext>
    </c:extLst>
  </c:chart>
</c:chartSpace>)");

        CHECK(errors == "");
    }
}
