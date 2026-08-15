// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/Tools/OutputNaming.hpp"

#include <filesystem>
#include <string>

namespace
{
using namespace ExyokiOffice::Tools;

TEST_CASE("IsPlainOutputName accepts one ordinary file name [unit] [tools]")
{
    CHECK(IsPlainOutputName("image1.png"));
    CHECK(IsPlainOutputName("part_01.docx"));
    CHECK(IsPlainOutputName("zpráva – finální.docx"));
    CHECK(IsPlainOutputName(".gitignore"));
    CHECK(IsPlainOutputName("report~1.docx"));
    CHECK(IsPlainOutputName("nul-report.png"));
    CHECK(IsPlainOutputName("communication.docx"));
}

TEST_CASE("IsPlainOutputName refuses a name that would not stay one file [unit] [tools]")
{
    CHECK_FALSE(IsPlainOutputName(""));
    CHECK_FALSE(IsPlainOutputName("."));
    CHECK_FALSE(IsPlainOutputName(".."));
    CHECK_FALSE(IsPlainOutputName("../evil.png"));
    CHECK_FALSE(IsPlainOutputName("sub/evil.png"));
    CHECK_FALSE(IsPlainOutputName(R"(..\..\evil.png)"));

    // A colon names a drive or an alternate data stream; both hide the payload
    // somewhere other than the file the caller was told about.
    CHECK_FALSE(IsPlainOutputName("image1.png:hidden"));
    CHECK_FALSE(IsPlainOutputName("C:evil.png"));

    // Device names reach a device with or without an extension, and Windows
    // ignores what follows the first dot as well as trailing dots and spaces.
    CHECK_FALSE(IsPlainOutputName("NUL"));
    CHECK_FALSE(IsPlainOutputName("nul.png"));
    CHECK_FALSE(IsPlainOutputName("CON"));
    CHECK_FALSE(IsPlainOutputName("com1.docx"));
    CHECK_FALSE(IsPlainOutputName("LPT9"));
    CHECK_FALSE(IsPlainOutputName("aux .txt"));

    CHECK_FALSE(IsPlainOutputName("report.docx."));
    CHECK_FALSE(IsPlainOutputName("report.docx "));
    CHECK_FALSE(IsPlainOutputName(std::string("report\nname.png")));
}

TEST_CASE("MakePlainOutputName rewrites rather than fails [unit] [tools]")
{
    // An acceptable name is returned unchanged, so an ordinary export keeps
    // naming its files after the parts they came from.
    CHECK(MakePlainOutputName("image1", "media") == "image1");

    CHECK(MakePlainOutputName("image1:hidden", "media") == "image1_hidden");
    CHECK(MakePlainOutputName("sub/image1", "media") == "sub_image1");
    CHECK(MakePlainOutputName("image1.", "media") == "image1");

    // Nothing usable is left: the fallback takes over rather than the caller
    // losing the payload.
    CHECK(MakePlainOutputName("", "media") == "media");
    CHECK(MakePlainOutputName("..", "media") == "media");
    CHECK(MakePlainOutputName("NUL", "media") == "media");
    CHECK(MakePlainOutputName("nul.png", "media") == "media");

    // Whatever comes out is acceptable by the same rule that judged the input.
    for (const auto& name : {"image1", "image1:hidden", "..", "NUL", "", "   "})
    {
        CHECK(IsPlainOutputName(MakePlainOutputName(name, "media")));
    }
}

TEST_CASE("IsInsideDirectory decides containment component-wise [unit] [tools]")
{
    const std::filesystem::path directory("out/parts");

    CHECK(IsInsideDirectory(directory, "out/parts/part_01.docx"));
    CHECK(IsInsideDirectory(directory, "out/parts/nested/part_01.docx"));
    CHECK(IsInsideDirectory(directory, directory));
    CHECK(IsInsideDirectory("out/parts/", "out/parts/part_01.docx"));

    CHECK_FALSE(IsInsideDirectory(directory, "out/part_01.docx"));
    CHECK_FALSE(IsInsideDirectory(directory, "out/parts-elsewhere/part_01.docx"));
    CHECK_FALSE(IsInsideDirectory(directory, "out/parts/../../evil_01.docx"));
    CHECK_FALSE(IsInsideDirectory(directory, "out"));

    // A prefix that shares no component cannot be a parent, whichever way the
    // two paths were spelled.
    CHECK_FALSE(IsInsideDirectory("/tmp/out", "out/parts/part_01.docx"));
}

} // namespace
