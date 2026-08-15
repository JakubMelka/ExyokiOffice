// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/FileFormatVersions.h"

namespace ExyokiOffice::Detail
{

/**
 * @brief Orders an Office generation on a single scale.
 *
 * `FileFormatVersions` is a bit flag type, but availability is a question of "at
 * least this generation", not of set membership: metadata records the generation a
 * feature appeared in, and every later one keeps it. Ranking collapses the flags to
 * that order, with `None` meaning "no requirement".
 */
inline unsigned OfficeVersionRank(OpenXml::FileFormatVersions version) noexcept
{
    using V = OpenXml::FileFormatVersions;
    if (static_cast<bool>(version & V::Microsoft365))
    {
        return 7;
    }
    if (static_cast<bool>(version & V::Office2021))
    {
        return 6;
    }
    if (static_cast<bool>(version & V::Office2019))
    {
        return 5;
    }
    if (static_cast<bool>(version & V::Office2016))
    {
        return 4;
    }
    if (static_cast<bool>(version & V::Office2013))
    {
        return 3;
    }
    if (static_cast<bool>(version & V::Office2010))
    {
        return 2;
    }
    if (static_cast<bool>(version & V::Office2007))
    {
        return 1;
    }
    return 0;
}

/**
 * @brief Reports whether @p target covers everything @p required needs.
 *
 * A requirement of `None` is no requirement at all and is always satisfied.
 */
inline bool SupportsOfficeVersion(OpenXml::FileFormatVersions target,
                                  OpenXml::FileFormatVersions required) noexcept
{
    return required == OpenXml::FileFormatVersions::None ||
           OfficeVersionRank(target) >= OfficeVersionRank(required);
}

} // namespace ExyokiOffice::Detail
