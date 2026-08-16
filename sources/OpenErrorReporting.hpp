// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/Packaging/WordprocessingDocument.hpp"
#include "ExyokiOffice/ValidationResult.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

namespace ExyokiOffice::Packaging::Detail
{

/**
 * @file
 * @brief Filling in Packaging::OpenError, shared by all three document families.
 *
 * The three Open implementations fail at the same points for the same reasons,
 * and an out-parameter that is only sometimes written is exactly the kind of
 * thing that rots. Keeping the vocabulary here means one place decides what a
 * failed load is called and one place copies the diagnostics out before the
 * package that owns them is destroyed.
 */
class OpenErrorReporter final
{
public:
    OpenErrorReporter() = delete;

    /// Records @p code and @p message, plus the package's diagnostics when given one.
    static void Report(OpenError* error, OpenErrorCode code, std::string message,
                       const OpenXmlPackage* package = nullptr)
    {
        if (error == nullptr)
        {
            return;
        }

        error->Code = code;
        error->Message = std::move(message);
        if (package != nullptr)
        {
            error->Diagnostics = package->LastValidationResult();
        }
    }

    /// True when the token exists and is signalled.
    static bool Cancelled(const ICancellationToken* token)
    {
        return token != nullptr && token->IsCancelled();
    }

    /// Records the cancellation and returns true, so call sites stay one line.
    static bool ReportCancelled(OpenError* error)
    {
        Report(error, OpenErrorCode::Cancelled, "Opening was cancelled before it finished.");
        return true;
    }

    /**
     * @brief Names a failed load from what the loader reported about it.
     *
     * A load fails for three reasons that a caller has to tell apart: it was
     * cancelled, the bytes were not a package it could read, or they were and a
     * configured limit stopped it. The token is asked first because a load that
     * stops halfway through leaves a package that looks damaged in exactly the
     * way an unreadable one does, and reporting that as a corrupt file would
     * blame the document for the caller's own cancellation. Between the other
     * two only the diagnostics know which, and they are about to be discarded
     * with the package.
     */
    static void ReportLoadFailure(OpenError* error, const OpenXmlPackage& package, const std::string& source,
                                  const ICancellationToken* token = nullptr)
    {
        if (Cancelled(token))
        {
            ReportCancelled(error);
            return;
        }

        const auto& issues = package.LastValidationResult().Issues();
        const bool limit = std::any_of(issues.begin(), issues.end(),
                                       [](const ValidationIssue& issue)
                                       {
                                           return issue.Id == ValidationErrorId::OpcLimitExceeded ||
                                                  issue.Id == ValidationErrorId::XmlLimitExceeded;
                                       });

        if (limit)
        {
            Report(error, OpenErrorCode::LimitExceeded,
                   "The package exceeds the configured ZIP/XML limits" + Suffix(source) + ".", &package);
            return;
        }

        Report(error, OpenErrorCode::NotAPackage,
               "The bytes are not a readable Open XML package" + Suffix(source) +
                   ". An encrypted or password-protected document reads this way too.",
               &package);
    }

    /**
     * @brief Reports why a path could not be loaded, filesystem reasons first.
     *
     * "Not a package" is the answer of last resort here. A path that is not
     * there, and a path that is there but cannot be opened - the usual case
     * being a permission the process does not have, or a file another program
     * holds exclusively - are the caller's to fix and say nothing about the
     * bytes. Readability is settled by opening the file rather than by reading
     * its permission bits, because the bits do not account for ACLs, mandatory
     * locks or a filesystem that is simply gone; the file can of course change
     * between the failed load and this probe, and then the answer falls through
     * to whatever the loader saw.
     */
    static void ReportFileLoadFailure(OpenError* error, const OpenXmlPackage& package,
                                      const std::filesystem::path& path,
                                      const ICancellationToken* token = nullptr)
    {
        if (Cancelled(token))
        {
            ReportCancelled(error);
            return;
        }

        std::error_code status;
        if (!std::filesystem::is_regular_file(path, status))
        {
            Report(error, OpenErrorCode::FileNotFound, "No file to open at " + path.string() + ".", &package);
            return;
        }

        if (std::ifstream probe(path, std::ios::binary); !probe.is_open())
        {
            Report(error, OpenErrorCode::FileUnreadable,
                   "The file at " + path.string() +
                       " exists but could not be opened for reading. It may be locked by another "
                       "program, or the process may lack permission to read it.",
                   &package);
            return;
        }

        ReportLoadFailure(error, package, path.string(), token);
    }

private:
    static std::string Suffix(const std::string& source)
    {
        return source.empty() ? std::string() : (" (" + source + ")");
    }
};

} // namespace ExyokiOffice::Packaging::Detail
