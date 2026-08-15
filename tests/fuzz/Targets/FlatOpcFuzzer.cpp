// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "FuzzHarness.hpp"
#include "FuzzTargets.hpp"

#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/Packaging/PowerPointDocument.hpp"
#include "ExyokiOffice/Packaging/SpreadsheetDocument.hpp"
#include "ExyokiOffice/Packaging/WordprocessingDocument.hpp"
#include "ExyokiOffice/Tools/FlatOpcConverter.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace ExyokiOffice::Fuzz
{

/**
 * @brief Helpers for the Flat OPC target.
 *
 * A named class rather than an anonymous namespace, matching the convention
 * used throughout this repository.
 */
class FlatOpcFuzzHelpers
{
public:
    /// Collects the URIs of every part reachable in the package, deduplicated.
    static std::vector<std::string> CollectPartUris(const OpenXmlPackage& package)
    {
        std::vector<std::string> uris;
        for (const auto& part : package.Parts())
        {
            if (part)
            {
                uris.push_back(part->Uri());
            }
        }

        // GetXxxParts()/Parts() may expose the same part through more than one
        // relationship edge, so identity has to be established on unique URIs.
        std::sort(uris.begin(), uris.end());
        uris.erase(std::unique(uris.begin(), uris.end()), uris.end());
        return uris;
    }

    /// Loads package bytes with the fuzzing limits installed.
    static std::shared_ptr<OpenXmlPackage> Load(const std::vector<Byte>& bytes)
    {
        auto package = std::make_shared<OpenXmlPackage>();
        package->SetPackageLimits(SafeLimits());
        if (!package->LoadFromMemory(bytes))
        {
            return nullptr;
        }
        return package;
    }

    /// Runs the typed Open overloads, which apply their own OpenSettings path.
    static void OpenTyped(UInt8 family, const std::vector<Byte>& bytes)
    {
        Packaging::OpenSettings settings;
        settings.PackageLimits = SafeLimits();
        settings.OpcValidation = Packaging::OpcValidationMode::Tolerant;

        switch (family % 3)
        {
            case 0:
                (void)Packaging::WordDocument::Open(bytes, settings);
                break;
            case 1:
                (void)Packaging::ExcelDocument::Open(bytes, settings);
                break;
            default:
                (void)Packaging::PowerPointDocument::Open(bytes, settings);
                break;
        }
    }
};

int RunFlatOpc(const UInt8* data, Size size)
{
    if (size > kMaxInputSize || data == nullptr || size == 0)
    {
        return 0;
    }

    // The whole input is the Flat OPC document - no prefix byte is carved out
    // of it - so a corpus seed is a plain, readable, hand-editable XML file.
    // The typed-Open family is derived from the length instead, which mutation
    // varies just as freely as it would a selector byte.
    const auto family = static_cast<UInt8>(size % 3);
    const std::string_view flatOpcXml(reinterpret_cast<const char*>(data), size);

    auto converted = Tools::ConvertFromFlatOpc(flatOpcXml);
    if (!converted.Ok || converted.PackageBytes.empty())
    {
        return 0;
    }

    auto package = FlatOpcFuzzHelpers::Load(converted.PackageBytes);
    if (!package)
    {
        return 0;
    }

    const auto originalUris = FlatOpcFuzzHelpers::CollectPartUris(*package);

    // Round-trip invariant: a package that loaded once must survive being
    // written back out and read in again with the same set of parts. This is
    // where a loader that silently drops or duplicates a part shows up.
    const auto saved = package->SaveToMemory();
    if (!saved.empty())
    {
        auto reloaded = FlatOpcFuzzHelpers::Load(saved);
        EXYOKIOFFICE_FUZZ_CHECK(reloaded != nullptr,
                                "package loaded once but failed to reload after SaveToMemory");

        const auto reloadedUris = FlatOpcFuzzHelpers::CollectPartUris(*reloaded);
        EXYOKIOFFICE_FUZZ_CHECK(reloadedUris == originalUris,
                                "save/load round-trip changed the set of parts");

        // The second leg drives ConvertToFlatOpc and the base64 encoder, which
        // the first leg never reaches.
        auto flat = Tools::ConvertToFlatOpc(saved);
        if (flat.Ok && !flat.FlatOpcXml.empty())
        {
            auto again = Tools::ConvertFromFlatOpc(flat.FlatOpcXml);
            EXYOKIOFFICE_FUZZ_CHECK(again.Ok,
                                    "ConvertToFlatOpc emitted XML that ConvertFromFlatOpc rejects");
            if (!again.PackageBytes.empty())
            {
                (void)FlatOpcFuzzHelpers::Load(again.PackageBytes);
            }
        }

        FlatOpcFuzzHelpers::OpenTyped(family, saved);
    }

    return 0;
}

} // namespace ExyokiOffice::Fuzz
