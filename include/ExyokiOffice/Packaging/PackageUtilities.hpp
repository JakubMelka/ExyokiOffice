// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

namespace ExyokiOffice::Packaging
{

/**
 * @brief Reads the entire file into a byte buffer. Returns an empty buffer on failure.
 */
EXYOKIOFFICE_EXPORT std::vector<Byte> ReadFileFully(const std::filesystem::path& path);

/**
 * @brief Reads the entire stream into a byte buffer from the beginning.
 *
 * The caller is responsible for passing a stream that can be seeked and read.
 * Returns an empty buffer on failure.
 */
EXYOKIOFFICE_EXPORT std::vector<Byte> ReadStreamFully(std::iostream& stream);

/**
 * @brief Writes the provided buffer at the beginning of a writable stream.
 *
 * This helper does not guarantee truncation for arbitrary std::iostream
 * implementations. The caller is responsible for providing an empty/truncated
 * destination when trailing bytes must not survive. Returns false when writing
 * fails.
 */
EXYOKIOFFICE_EXPORT bool WriteStream(std::iostream& stream, const std::vector<Byte>& data);

/**
 * @brief Normalizes a file-system path so that it can be used as a relationship target.
 */
EXYOKIOFFICE_EXPORT std::string BuildRelationshipTarget(const std::filesystem::path& path);

} // namespace ExyokiOffice::Packaging
