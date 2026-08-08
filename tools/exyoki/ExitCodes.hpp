// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include <array>

namespace exyoki
{

/// Process exit codes returned by exyoki. The numeric values are part of the
/// tool's contract and are published by the `commands` catalog.
enum class ExitCode : int
{
    Ok = 0,
    OperationFailed = 1,
    UsageError = 2,
    ValidationErrors = 3,
    DiffDifferent = 4,
    SearchNoMatch = 5,
    QueryNoMatch = 6,
    UnhandledException = 7,
    SignatureInvalid = 8
};

/// One row of the published exit code table.
struct ExitCodeDescription
{
    ExitCode Code;
    /// Stable machine-readable identifier, never translated or reworded.
    const char* Name;
    const char* Meaning;
};

/// Every exit code exyoki can return, in ascending numeric order.
inline constexpr std::array<ExitCodeDescription, 9> AllExitCodes{
    {{ExitCode::Ok, "ok", "The command completed successfully."},
     {ExitCode::OperationFailed, "operationFailed",
      "The operation failed: missing file, I/O error, corrupt package, or a document family the command does not "
      "support."},
     {ExitCode::UsageError, "usageError", "The command line could not be parsed or the arguments are inconsistent."},
     {ExitCode::ValidationErrors, "validationErrors",
      "validate found at least one error (or a warning, with --warnings-as-errors)."},
     {ExitCode::DiffDifferent, "diffDifferent", "diff found at least one difference between the two packages."},
     {ExitCode::SearchNoMatch, "searchNoMatch", "search completed but matched nothing (grep-like convention)."},
     {ExitCode::QueryNoMatch, "queryNoMatch", "query completed but matched no nodes (grep-like convention)."},
     {ExitCode::UnhandledException, "unhandledException", "An unexpected exception escaped the command."},
     {ExitCode::SignatureInvalid, "signatureInvalid",
      "signatures found a signature whose signed content has changed."}}};

} // namespace exyoki
