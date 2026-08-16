// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/Excel/ExcelCellValue.hpp"
#include "ExyokiOffice/OpenXmlSimpleTypes.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <clocale>
#include <string>
#include <string_view>

/**
 * @file
 * @brief Numbers in a document mean the same thing whatever locale the host set.
 *
 * A library cannot decide what `setlocale` the hosting application calls, and an
 * Office file is not written in the user's locale: `1.5` is one and a half in
 * every `.docx` on earth. Reading or writing one through the C conversions made
 * that depend on the process - under a German locale the same workbook wrote
 * `1,5` into `<v>`, which Excel refuses, and read `1.5` back as `1`.
 *
 * Each case installs a locale whose decimal separator is a comma and restores
 * the previous one afterwards. Where no such locale is available - a container
 * with no locale data - the case still runs and asserts the same values: that
 * the answer never changes is the whole point.
 */
class LocaleGuard
{
public:
    LocaleGuard()
    {
        const char* previous = std::setlocale(LC_ALL, nullptr);
        m_previous = previous ? previous : "C";
        // Several spellings, because the name differs between platforms and any
        // one of them proves the point.
        for (const char* candidate : {"de_DE.UTF-8", "de_DE", "de-DE", "German_Germany.1252"})
        {
            if (std::setlocale(LC_ALL, candidate) != nullptr)
            {
                m_installed = true;
                break;
            }
        }
    }

    ~LocaleGuard()
    {
        std::setlocale(LC_ALL, m_previous.c_str());
    }

    LocaleGuard(const LocaleGuard&) = delete;
    LocaleGuard& operator=(const LocaleGuard&) = delete;
    LocaleGuard(LocaleGuard&&) = delete;
    LocaleGuard& operator=(LocaleGuard&&) = delete;

    /// True when a comma-separator locale was actually installed.
    [[nodiscard]] bool Installed() const noexcept { return m_installed; }

private:
    std::string m_previous;
    bool m_installed = false;
};

TEST_SUITE("Unit.LocaleIndependentNumbers")
{
    TEST_CASE("xsd:decimal parses and formats with a point under any locale [unit] [locale]")
    {
        const LocaleGuard guard;
        INFO("comma-separator locale installed: " << guard.Installed());

        ExyokiOffice::DecimalValue parsed(std::string_view("1234.5"));
        REQUIRE(parsed.IsDefined());
        CHECK(static_cast<double>(parsed.Value()) == doctest::Approx(1234.5));

        const auto formatted = ExyokiOffice::DecimalValue(static_cast<ExyokiOffice::RealExtended>(1.5L)).ToString();
        CHECK(formatted.find(',') == std::string::npos);
        CHECK(formatted == "1.5");

        // A comma is not a decimal separator in XML, whatever it is in the shell
        // the program was started from.
        const ExyokiOffice::DecimalValue commaValue(std::string_view("1,5"));
        CHECK_FALSE(commaValue.IsDefined());
    }

    TEST_CASE("A number cell is written with a point under any locale [unit] [locale]")
    {
        const LocaleGuard guard;
        INFO("comma-separator locale installed: " << guard.Installed());

        const auto value = ExyokiOffice::Excel::ExcelCellValue::Number(1234.5);
        CHECK(value.Text() == "1234.5");
        CHECK(value.Text().find(',') == std::string::npos);
    }
}
