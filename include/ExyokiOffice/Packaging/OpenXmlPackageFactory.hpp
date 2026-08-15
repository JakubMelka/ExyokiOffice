// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include <memory>
#include <string_view>

namespace ExyokiOffice
{

class OpenXmlPackage;
class OpenXmlPackagePart;

} // namespace ExyokiOffice

namespace ExyokiOffice::Generated
{

std::shared_ptr<OpenXmlPackagePart> CreatePackagePart(std::string_view relationshipType,
                                                      std::string_view contentType,
                                                      OpenXmlPackage& package);

} // namespace ExyokiOffice::Generated
