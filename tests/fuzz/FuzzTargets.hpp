// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/StandardTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace ExyokiOffice::Fuzz
{

/**
 * @brief Signature shared by every fuzz entry point.
 *
 * The return value exists only because libFuzzer requires it; targets always
 * return 0. A target reports a defect by crashing, by tripping an assertion, or
 * by letting a sanitizer fire - never through the return value.
 */
using FuzzEntryPoint = int (*)(const UInt8* data, Size size);

/**
 * @brief Flat OPC text to a fully loaded package.
 *
 * The primary target. A Flat OPC document is the whole package expressed as a
 * single XML file, so the fuzzer mutates text rather than a ZIP container and
 * never has to satisfy a CRC. ConvertFromFlatOpc rebuilds a valid archive from
 * that text, which then goes through the regular package loader.
 */
int RunFlatOpc(const UInt8* data, Size size);

/**
 * @brief Raw bytes to OpenXmlPackage::LoadFromMemory.
 *
 * Secondary and deliberately small. Random mutation of a ZIP fails the CRC
 * check long before any part is parsed, so this target mostly exercises the
 * archive framing and the hand-written limit checks around it, not the OPC or
 * DOM layers.
 */
int RunPackageLoad(const UInt8* data, Size size);

/// Raw XML into a package part, plus the XPath expression parser.
int RunXmlPart(const UInt8* data, Size size);

/// The OpenXmlSimpleValueConvertor from-string parsers for every simple type.
int RunSimpleTypes(const UInt8* data, Size size);

/// Excel formula, cell address and range parsing plus reference rewriting.
int RunFormula(const UInt8* data, Size size);

/// Part graph cloning and DOM structural operations driven by a command tape.
int RunCopyOps(const UInt8* data, Size size);

#if defined(EXYOKIOFFICE_FUZZ_HAS_MCP)
/**
 * @brief JSON-RPC lines into McpServer::HandleMessage.
 *
 * The only surface an untrusted MCP client reaches directly: the line parser,
 * the JSON Schema validation of every tool argument, and the handlers behind
 * them. The workspace points at a directory that does not exist, so a run
 * cannot leave state behind for the next input.
 *
 * Present only when the MCP servers are part of the build.
 */
int RunMcpRpc(const UInt8* data, Size size);
#endif

/// One named fuzz entry point.
struct FuzzTargetInfo
{
    /// Short name; also the corpus, dictionary and crash directory name.
    std::string_view Name;
    FuzzEntryPoint Entry;
};

/**
 * @brief All registered targets.
 *
 * Used by the corpus replay unit test to map a corpus directory to the entry
 * point that owns it. The libFuzzer executables do not go through this table -
 * each of them links exactly one entry point directly.
 */
std::span<const FuzzTargetInfo> AllTargets() noexcept;

} // namespace ExyokiOffice::Fuzz
