// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "FuzzHarness.hpp"
#include "FuzzTargets.hpp"

#include "ExyokiOffice/OpenXMLElement.hpp"
#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/Tools/FlatOpcConverter.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <memory>
#include <vector>

namespace ExyokiOffice::Fuzz
{

/**
 * @brief Part graph copying and DOM structural editing.
 *
 * The package shape comes from Flat OPC text, so the fuzzer can build arbitrary
 * relationship graphs - cycles, shared children, dangling targets - without
 * having to satisfy a ZIP container. The structural operations are then driven
 * by a command tape, which is what makes this target interesting: the failure
 * mode being hunted is not a single bad call but a sequence where one operation
 * leaves a wrapper pointing at a node another operation already destroyed.
 */
class CopyOpsFuzzHelpers
{
public:
    /// Collects the root element plus its descendants, breadth first and bounded.
    static std::vector<std::shared_ptr<OpenXMLElement>>
    CollectElements(const std::shared_ptr<OpenXMLElement>& root)
    {
        std::vector<std::shared_ptr<OpenXMLElement>> elements;
        if (!root)
        {
            return elements;
        }

        elements.push_back(root);
        for (Size index = 0; index < elements.size() && elements.size() < kMaxElements; ++index)
        {
            for (const auto& child : elements[index]->Children())
            {
                if (child)
                {
                    elements.push_back(child);
                }
            }
        }

        return elements;
    }

    /// Picks an element from the pool; returns nullptr for an empty pool.
    static std::shared_ptr<OpenXMLElement>
    Pick(const std::vector<std::shared_ptr<OpenXMLElement>>& pool, UInt8 selector)
    {
        if (pool.empty())
        {
            return nullptr;
        }
        return pool[selector % pool.size()];
    }

    /// Returns the root element of the first XML part that has one.
    static std::shared_ptr<OpenXMLElement> FirstRootElement(const OpenXmlPackage& package)
    {
        for (const auto& part : package.Parts())
        {
            if (!part)
            {
                continue;
            }

            if (auto root = part->GetRootElement())
            {
                return root;
            }
        }
        return nullptr;
    }

private:
    /// Keeps one iteration cheap; deep documents would starve the fuzzer.
    static constexpr Size kMaxElements = 512;
};

int RunCopyOps(const UInt8* data, Size size)
{
    if (size > kMaxInputSize)
    {
        return 0;
    }

    ByteTape tape(data, size);
    const UInt8 operationCount = tape.NextChoice(16);

    // Reserve the command tape before the payload, so mutating the trailing XML
    // does not shift the operation sequence.
    UInt8 commands[16 * 3] = {};
    for (auto& command : commands)
    {
        command = tape.NextByte();
    }

    auto converted = Tools::ConvertFromFlatOpc(tape.RestAsText());
    if (!converted.Ok || converted.PackageBytes.empty())
    {
        return 0;
    }

    auto source = std::make_shared<OpenXmlPackage>();
    source->SetPackageLimits(SafeLimits());
    if (!source->LoadFromMemory(converted.PackageBytes))
    {
        return 0;
    }

    // Part graph import into a second, initially empty package.
    auto destination = std::make_shared<OpenXmlPackage>();
    destination->SetPackageLimits(SafeLimits());
    for (const auto& part : source->Parts())
    {
        if (part)
        {
            (void)destination->ImportPartGraph(part);
            (void)source->ClonePartGraph(part);
        }
    }

    auto root = CopyOpsFuzzHelpers::FirstRootElement(*source);
    if (!root)
    {
        return 0;
    }

    for (UInt8 operation = 0; operation < operationCount; ++operation)
    {
        // Re-collecting every round is deliberate: the pool must never outlive
        // the structural change that invalidated it, which is exactly the bug
        // class this target exists to find.
        const auto pool = CopyOpsFuzzHelpers::CollectElements(root);
        const UInt8* command = commands + (Size{operation} * 3);

        auto subject = CopyOpsFuzzHelpers::Pick(pool, command[1]);
        auto other = CopyOpsFuzzHelpers::Pick(pool, command[2]);
        if (!subject || !other || subject == other)
        {
            continue;
        }

        switch (command[0] % 7)
        {
            case 0:
                (void)subject->CopyInto(other);
                break;
            case 1:
                (void)subject->CopyAfter(other);
                break;
            case 2:
                (void)subject->MoveInto(other);
                break;
            case 3:
                (void)subject->MoveAfter(other);
                break;
            case 4:
                (void)subject->ReplaceWith(other);
                break;
            case 5:
                (void)subject->ReplaceWithChildren();
                break;
            default:
                // The root itself has no parent to be removed from; keeping it
                // alive is what lets the loop keep running.
                if (subject != root)
                {
                    (void)subject->Remove();
                }
                break;
        }
    }

    // Whatever the edits produced still has to serialize.
    (void)source->SaveToMemory();

    return 0;
}

} // namespace ExyokiOffice::Fuzz
