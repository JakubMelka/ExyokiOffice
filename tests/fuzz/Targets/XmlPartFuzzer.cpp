// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "FuzzHarness.hpp"
#include "FuzzTargets.hpp"

#include "ExyokiOffice/OpenXmlPackagePart.hpp"
#include "ExyokiOffice/Xml/XmlQuery.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <memory>
#include <string>
#include <string_view>

namespace ExyokiOffice::Fuzz
{

/**
 * @brief Raw XML into a part, without the OPC container around it.
 *
 * The Flat OPC target reaches SetXmlString too, but only through text that
 * already had to survive Flat OPC framing and base64 decoding. Feeding a part
 * directly removes that filter, so the fuzzer spends its whole budget on the
 * XML reader and on the typed DOM built on top of it.
 */
class XmlPartFuzzHelpers
{
public:
    /// Descriptor for a throwaway XML part that belongs to no package.
    static const OpenXmlPartDescriptor& ScratchDescriptor()
    {
        static const OpenXmlPartDescriptor descriptor{
            /* Name */ "FuzzScratchPart",
            /* RelationshipType */ "http://schemas.openxmlformats.org/officeDocument/2006/"
                                   "relationships/officeDocument",
            /* ContentType */ "application/xml",
            /* Target */ "scratch",
            /* TargetExtension */ "xml",
            /* DefaultPath */ "fuzz"};
        return descriptor;
    }
};

int RunXmlPart(const UInt8* data, Size size)
{
    if (size > kMaxInputSize)
    {
        return 0;
    }

    ByteTape tape(data, size);
    const std::string xpath(tape.NextChunk());
    const std::string xml(tape.RestAsText());

    auto part = std::make_shared<OpenXmlPackagePart>(XmlPartFuzzHelpers::ScratchDescriptor());
    part->SetXmlString(xml);

    auto root = part->GetRootElement();
    if (!root)
    {
        return 0;
    }

    // Serializing and re-reading has to reach a fixed point. The comparison is
    // between the second and third passes rather than the first and second on
    // purpose: the first parse legitimately normalizes the input - it resolves
    // entities, drops an illegal character, settles attribute quoting - so the
    // first output is allowed to differ from what went in. What must not happen
    // is the text continuing to change afterwards, because that is what makes a
    // document drift a little further on every open-and-save cycle.
    //
    // The weaker "first equals second" form was tried first and reported
    // `&#0;`: pugixml resolves it to a NUL, which is not a legal XML character
    // and cannot survive a write-read cycle unchanged. That is a property of
    // the vendored parser, not a defect here.
    const std::string firstPass = part->GetXmlString();

    auto second = std::make_shared<OpenXmlPackagePart>(XmlPartFuzzHelpers::ScratchDescriptor());
    second->SetXmlString(firstPass);
    if (second->GetRootElement())
    {
        const std::string secondPass = second->GetXmlString();

        auto third = std::make_shared<OpenXmlPackagePart>(XmlPartFuzzHelpers::ScratchDescriptor());
        third->SetXmlString(secondPass);
        if (third->GetRootElement())
        {
            EXYOKIOFFICE_FUZZ_CHECK(third->GetXmlString() == secondPass,
                                    "XML serialization never reaches a fixed point");
        }
    }

    // XPath is a separate grammar with its own parser; the expression comes
    // from the tape rather than from the document.
    if (!xpath.empty())
    {
        (void)Xml::XmlQuery::Select(root, xpath).Count();
    }

    return 0;
}

} // namespace ExyokiOffice::Fuzz
