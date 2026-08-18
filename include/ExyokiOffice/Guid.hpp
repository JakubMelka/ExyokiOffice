// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"

#include <string>
#include <string_view>

namespace ExyokiOffice
{

/** @brief Generates GUID identifiers in the representation used by OOXML. */
class EXYOKIOFFICE_EXPORT Guid final
{
public:
    Guid() = delete;

    /**
     * @brief Creates a random RFC 4122 version-4 GUID.
     *
     * The returned value uses uppercase hexadecimal digits and braces, for
     * example `{00112233-4455-4677-8899-AABBCCDDEEFF}`.
     */
    [[nodiscard]] static std::string New();

    /**
     * @brief Tells whether @p value is a braced GUID, `{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}`.
     *
     * Hexadecimal digits of either case are accepted; this is the form Office
     * expects wherever a schema or an application reads an identifier as a GUID.
     */
    [[nodiscard]] static bool IsBraced(std::string_view value) noexcept;
};

} // namespace ExyokiOffice
