// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Guid.hpp"

#include <doctest/doctest.h>

#include <regex>
#include <set>
#include <string>

using namespace ExyokiOffice;

TEST_CASE("GUID generation uses the OOXML-compatible RFC 4122 version-4 form [unit] [guid]")
{
    const std::regex pattern(
        R"(^\{[0-9A-F]{8}-[0-9A-F]{4}-4[0-9A-F]{3}-[89AB][0-9A-F]{3}-[0-9A-F]{12}\}$)");
    std::set<std::string> values;

    for (int index = 0; index < 256; ++index)
    {
        const auto value = Guid::New();
        CAPTURE(value);
        CHECK(std::regex_match(value, pattern));
        CHECK(values.insert(value).second);
    }
}
