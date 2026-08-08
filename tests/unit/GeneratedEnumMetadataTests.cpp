// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

// The generated enum metadata has to be reachable from outside the shared
// library. `OpenXmlEnumTraits::TryParse` and `Format` are header-only
// templates that call `TEnum::GetMetaEnum()`, so every consumer that parses or
// formats an `EnumValue<T>` — a tool, a test, an application — resolves that
// symbol against the DLL's export table. This layer links ExyokiOffice the
// same way a consumer does, so the cases below fail to link, not merely to
// assert, if the accessor ever loses its EXYOKIOFFICE_EXPORT.

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Presentation.Enums.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Spreadsheet.Enums.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Wordprocessing.Enums.hpp"
#include "ExyokiOffice/OpenXmlSimpleTypes.hpp"

#include <doctest/doctest.h>

#include <string>

using namespace ExyokiOffice;

namespace W = ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing;
namespace S = ExyokiOffice::DocumentFormat::OpenXml::Spreadsheet;
namespace P = ExyokiOffice::DocumentFormat::OpenXml::Presentation;

TEST_CASE("generated enum metadata is reachable from outside the library [enum-metadata]")
{
    const auto* highlight = W::HighlightColorValues::GetMetaEnum();
    REQUIRE(highlight != nullptr);
    CHECK(highlight->ToString(static_cast<UInt32>(W::HighlightColorValues::Yellow)) == "yellow");
    CHECK(highlight->FromString("darkCyan") == static_cast<UInt32>(W::HighlightColorValues::DarkCyan));

    const auto* alignment = W::JustificationValues::GetMetaEnum();
    REQUIRE(alignment != nullptr);
    CHECK(alignment->ToString(static_cast<UInt32>(W::JustificationValues::Both)) == "both");

    const auto* placeholder = P::PlaceholderValues::GetMetaEnum();
    REQUIRE(placeholder != nullptr);
    CHECK(placeholder->ToString(static_cast<UInt32>(P::PlaceholderValues::CenteredTitle)) == "ctrTitle");
}

TEST_CASE("an unknown token resolves to the invalid enumerator [enum-metadata]")
{
    const auto* highlight = W::HighlightColorValues::GetMetaEnum();
    REQUIRE(highlight != nullptr);

    const auto raw = highlight->FromString("definitely-not-a-highlight");
    const W::HighlightColorValues parsed(static_cast<W::HighlightColorValues::Value>(raw));
    CHECK_FALSE(parsed.IsValid());
}

TEST_CASE("EnumValue parses and formats through the metadata [enum-metadata]")
{
    // This is the path a consumer actually takes: the traits template is
    // header-only and instantiated in the caller's translation unit.
    EnumValue<W::HighlightColorValues> highlight{std::string_view("green")};
    REQUIRE(highlight.IsDefined());
    CHECK(highlight.Value().GetValue() == W::HighlightColorValues::Green);
    CHECK(highlight.ToString() == "green");

    EnumValue<S::CellValues> cell{std::string_view("inlineStr")};
    REQUIRE(cell.IsDefined());
    CHECK(cell.ToString() == "inlineStr");

    EnumValue<W::HighlightColorValues> rejected{std::string_view("chartreuse")};
    CHECK_FALSE(rejected.IsDefined());
}
