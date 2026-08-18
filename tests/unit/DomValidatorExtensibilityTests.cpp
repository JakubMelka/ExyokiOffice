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

#include "DomValidationTestSupport.hpp"

#include "doctest.h"

TEST_SUITE("DOM validation of open-ended content models")
{
    using ExyokiOfficeTests::DomValidation::ValidationErrors;

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
  <c:chart><c:plotArea><c:pieChart/></c:plotArea></c:chart>
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
  <c:chart><c:plotArea><c:pieChart/></c:plotArea></c:chart>
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
  <c:chart><c:plotArea><c:pieChart/></c:plotArea></c:chart>
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
    <c:plotArea><c:layout/><c:pieChart/></c:plotArea>
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
