// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Tools/DocumentModel.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <nlohmann/json.hpp>

namespace ExyokiOffice::Tools::detail
{

/// Builds the canonical envelope tree shared by the JSON and semantic-XML formats.
nlohmann::ordered_json BuildModelTree(const DocumentModel& model, bool embedMedia);

/// Rebuilds a model from the canonical envelope tree (tolerant; see docs/tools/conversion-formats.md).
DocumentModel ParseModelTree(const nlohmann::ordered_json& tree, std::vector<ToolDiagnostic>& diagnostics);

std::string EncodeBase64(const std::vector<Byte>& bytes);
bool DecodeBase64(std::string_view text, std::vector<Byte>& out);

} // namespace ExyokiOffice::Tools::detail
