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

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Drawing.Enums.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Math.Enums.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Presentation.Enums.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Spreadsheet.Enums.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Wordprocessing.Enums.hpp"
#include "ExyokiOffice/OpenXmlSimpleTypes.hpp"

#include <doctest/doctest.h>

#include <string>
#include <string_view>

using namespace ExyokiOffice;

namespace W = ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing;
namespace S = ExyokiOffice::DocumentFormat::OpenXml::Spreadsheet;
namespace P = ExyokiOffice::DocumentFormat::OpenXml::Presentation;
namespace A = ExyokiOffice::DocumentFormat::OpenXml::Drawing;
namespace M = ExyokiOffice::DocumentFormat::OpenXml::Math;

/**
 * The contract every generated enum shares, checked through one instantiation.
 *
 * The generator stamps the same shape into more than a thousand enum classes:
 * `NotDefinedEnumValue` and `InvalidEnumValue` ahead of the schema tokens,
 * `IsValid()` excluding exactly those two, `FromString` answering an unknown
 * token with Invalid - never with the first schema member - and `ToString`
 * refusing to spell either sentinel. An unknown token is not an exotic input:
 * it is what any newer Office writes when its schema knows a value this
 * import does not, and mapping it to a real enumerator would silently change
 * the document. One check per namespace pins the contract for that namespace's
 * generated file; sweeping all thousand instantiations would only repeat the
 * same generated line.
 */
class GeneratedEnumContract final
{
public:
    GeneratedEnumContract() = delete;

    template <typename TEnum>
    static void Check(std::string_view validToken)
    {
        const auto* meta = TEnum::GetMetaEnum();
        REQUIRE(meta != nullptr);

        // A schema token parses to a valid member and formats back to itself.
        const auto raw = meta->FromString(validToken);
        const TEnum parsed(static_cast<typename TEnum::Value>(raw));
        REQUIRE_MESSAGE(parsed.IsValid(), "token: ", validToken);
        CHECK_FALSE(parsed.isUndefined());
        CHECK_FALSE(parsed.isInvalid());
        CHECK(meta->ToString(raw) == validToken);

        // An unknown token resolves to the invalid enumerator specifically.
        const auto unknownRaw = meta->FromString("never-a-schema-token");
        const TEnum unknown(static_cast<typename TEnum::Value>(unknownRaw));
        CHECK_FALSE(unknown.IsValid());
        CHECK(unknown.isInvalid());
        CHECK_FALSE(unknown.isUndefined());

        // A default-constructed value is absent, which is distinct from invalid.
        const TEnum defaulted;
        CHECK_FALSE(defaulted.IsValid());
        CHECK(defaulted.isUndefined());
        CHECK_FALSE(defaulted.isInvalid());

        // Neither sentinel has a spelling.
        CHECK(meta->ToString(static_cast<UInt32>(TEnum::NotDefinedEnumValue)).empty());
        CHECK(meta->ToString(static_cast<UInt32>(TEnum::InvalidEnumValue)).empty());

        // The header-only traits path a consumer takes agrees with the metadata.
        const EnumValue<TEnum> viaTraits{validToken};
        REQUIRE(viaTraits.IsDefined());
        CHECK(viaTraits.ToString() == validToken);
        const EnumValue<TEnum> rejected{std::string_view("never-a-schema-token")};
        CHECK_FALSE(rejected.IsDefined());
    }
};

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

    // The traits path maps a token to the concrete enumerator, not merely to
    // some valid one.
    EnumValue<W::HighlightColorValues> highlightViaTraits{std::string_view("green")};
    REQUIRE(highlightViaTraits.IsDefined());
    CHECK(highlightViaTraits.Value().GetValue() == W::HighlightColorValues::Green);
}

// The unknown-token and EnumValue round-trip behavior of every namespace is
// pinned by GeneratedEnumContract::Check below; the case above keeps only the
// assertions the shared check does not make.
TEST_CASE("every generated namespace honors the shared enum contract [enum-metadata]")
{
    // Wordprocessing: a rich token list and the two-member degenerate case.
    GeneratedEnumContract::Check<W::HighlightColorValues>("darkCyan");
    GeneratedEnumContract::Check<W::JustificationValues>("both");
    GeneratedEnumContract::Check<W::OnOffOnlyValues>("off");

    // Spreadsheet: cell types and the sheet visibility triple.
    GeneratedEnumContract::Check<S::CellValues>("inlineStr");
    GeneratedEnumContract::Check<S::SheetStateValues>("visible");

    // Presentation.
    GeneratedEnumContract::Check<P::PlaceholderValues>("ctrTitle");

    // DrawingML.
    GeneratedEnumContract::Check<A::TextAlignmentTypeValues>("ctr");
    GeneratedEnumContract::Check<A::RectangleAlignmentValues>("ctr");

    // Office Math.
    GeneratedEnumContract::Check<M::FractionTypeValues>("bar");
}
