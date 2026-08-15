// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "FuzzHarness.hpp"
#include "FuzzTargets.hpp"

#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <memory>
#include <span>

namespace ExyokiOffice::Fuzz
{

/**
 * @brief Raw package bytes straight into the loader.
 *
 * Deliberately the smallest target in the set. Mutating a real .docx breaks the
 * per-entry CRC32 that miniz verifies in mz_zip_reader_extract_to_heap, so the
 * fuzzer rarely gets past the archive framing and into any part. What it does
 * reach is worth covering anyway: the central directory walk, entry name
 * handling, and the hand-written ValidateZipMetadata / CheckCurrentEntryLimits
 * checks layered on top of miniz. Deep coverage of the OPC and DOM layers is
 * the Flat OPC target's job.
 */
int RunPackageLoad(const UInt8* data, Size size)
{
    if (size > kMaxInputSize || data == nullptr)
    {
        return 0;
    }

    auto package = std::make_shared<OpenXmlPackage>();
    package->SetPackageLimits(SafeLimits());

    if (package->LoadFromMemory(std::span<const Byte>(data, size)))
    {
        // Anything that survived the loader has to survive being written back.
        (void)package->SaveToMemory();
    }

    return 0;
}

} // namespace ExyokiOffice::Fuzz
