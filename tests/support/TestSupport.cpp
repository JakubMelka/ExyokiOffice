// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "TestSupport.hpp"

#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/OpenXmlPackageValidator.hpp"
#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"
#include "ExyokiOffice/Tools/PackageDiff.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace ExyokiOfficeTests
{

/// File-local helpers. The repository avoids anonymous namespaces, so these sit
/// on a class with static members instead.
class TestSupportHelpers
{
public:
    static int CurrentProcessId()
    {
#ifdef _WIN32
        return _getpid();
#else
        return static_cast<int>(::getpid());
#endif
    }

    /// True when @p name is present in the environment and not empty.
    static bool IsEnvironmentVariableSet(const char* name)
    {
#ifdef _WIN32
        // getenv is deprecated under the MSVC CRT and the test layers compile
        // with warnings as errors.
        char* value = nullptr;
        size_t size = 0;
        if (_dupenv_s(&value, &size, name) != 0 || value == nullptr)
        {
            return false;
        }

        const bool present = value[0] != '\0';
        std::free(value);
        return present;
#else
        const char* value = std::getenv(name);
        return value != nullptr && value[0] != '\0';
#endif
    }

    static bool WriteBytes(const std::filesystem::path& path, std::span<const ExyokiOffice::Byte> bytes)
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream)
        {
            return false;
        }
        if (!bytes.empty())
        {
            stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        stream.close();
        return stream.good();
    }

    static std::string DescribePartChange(const ExyokiOffice::Tools::PartDiffEntry& entry)
    {
        std::string kind;
        switch (entry.Kind)
        {
            case ExyokiOffice::Tools::PartChangeKind::Added:
                kind = "added";
                break;
            case ExyokiOffice::Tools::PartChangeKind::Removed:
                kind = "removed";
                break;
            case ExyokiOffice::Tools::PartChangeKind::ChangedXml:
                kind = "changed XML";
                break;
            case ExyokiOffice::Tools::PartChangeKind::ChangedBinary:
                kind = "changed bytes";
                break;
            case ExyokiOffice::Tools::PartChangeKind::ContentTypeChanged:
                kind = "changed content type";
                break;
        }

        auto message = "part " + entry.Uri + ": " + kind;
        if (!entry.FirstDifferencePath.empty())
        {
            message += " at " + entry.FirstDifferencePath;
        }
        return message;
    }

    static std::string DescribeRelationshipChange(const ExyokiOffice::Tools::RelationshipDiffEntry& entry)
    {
        std::string kind;
        switch (entry.Kind)
        {
            case ExyokiOffice::Tools::RelationshipChangeKind::Added:
                kind = "added";
                break;
            case ExyokiOffice::Tools::RelationshipChangeKind::Removed:
                kind = "removed";
                break;
            case ExyokiOffice::Tools::RelationshipChangeKind::Changed:
                kind = "changed";
                break;
        }

        return "relationship " + entry.RelationshipId + " of " + entry.SourceUri + ": " + kind;
    }
};

/**
 * @brief The one directory every temporary test path lives in.
 *
 * Cleanup used to be each case's own business, and most cases do end with a
 * `std::filesystem::remove` over the paths they made. That line is exactly the
 * one a failing REQUIRE skips, though: the case leaves immediately, and the
 * artifact of the run that went wrong is the one left behind in the system
 * temporary directory. A new case forgetting the line has the same effect, and
 * neither shows up in a passing suite.
 *
 * Collecting every path under one root turns cleanup into a single remove_all
 * in this object's destructor, which runs after doctest returns from main
 * whether the cases passed, failed, or threw. The per-case removals stay
 * useful for keeping the root small during a long run, but nothing depends on
 * them any more.
 *
 * The name carries the process id and a clock reading: `ctest -j` runs the
 * layers concurrently, and two executables started in the same tick would
 * otherwise share a root and delete each other's files on exit.
 */
class TemporaryRootDirectory
{
public:
    TemporaryRootDirectory()
        : m_keep(TestSupportHelpers::IsEnvironmentVariableSet("EXYOKIOFFICE_TESTS_KEEP_TEMP"))
    {
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        std::string name = "exyokioffice-tests-";
        name += std::to_string(TestSupportHelpers::CurrentProcessId());
        name += '-';
        name += std::to_string(static_cast<unsigned long long>(ticks));

        std::error_code error;
        auto base = std::filesystem::temp_directory_path(error);
        if (error)
        {
            base = std::filesystem::current_path(error);
        }

        m_path = base / name;
        std::filesystem::create_directories(m_path, error);
    }

    ~TemporaryRootDirectory()
    {
        if (m_keep)
        {
            std::cout << "Temporary test files kept in " << m_path.string() << '\n';
            return;
        }

        std::error_code ignored;
        std::filesystem::remove_all(m_path, ignored);
    }

    TemporaryRootDirectory(const TemporaryRootDirectory&) = delete;
    TemporaryRootDirectory& operator=(const TemporaryRootDirectory&) = delete;
    TemporaryRootDirectory(TemporaryRootDirectory&&) = delete;
    TemporaryRootDirectory& operator=(TemporaryRootDirectory&&) = delete;

    /// Creates the root on first use; thread-safe through the magic static.
    static const std::filesystem::path& Path()
    {
        static const TemporaryRootDirectory root;
        return root.m_path;
    }

private:
    std::filesystem::path m_path;
    bool m_keep = false;
};

std::filesystem::path MakeTemporaryPath(std::string_view stem, std::string_view extension)
{
    static std::atomic<unsigned long long> counter{0};

    // The root already carries the process id and a tick, so the leaf only has
    // to stay unique within this process.
    auto name = std::string(stem);
    name += '_';
    name += std::to_string(counter.fetch_add(1));
    name += std::string(extension);

    return TemporaryRootDirectory::Path() / name;
}

ScopedTemporaryFile::ScopedTemporaryFile(std::string_view stem, std::string_view extension)
    : m_path(MakeTemporaryPath(stem, extension))
{
}

ScopedTemporaryFile::~ScopedTemporaryFile()
{
    std::error_code ignored;
    std::filesystem::remove_all(m_path, ignored);
}

ValidationSummary ValidatePackage(std::span<const ExyokiOffice::Byte> packageBytes)
{
    ValidationSummary summary;

    ExyokiOffice::OpenXmlPackage package;
    if (!package.LoadFromMemory(packageBytes))
    {
        return summary;
    }
    summary.Loaded = true;

    const auto result = ExyokiOffice::OpenXmlPackageValidator().Validate(package);
    for (const auto& issue : result.Issues())
    {
        if (issue.Severity == ExyokiOffice::ValidationSeverity::Error)
        {
            if (summary.ErrorCount == 0)
            {
                summary.FirstError = issue.Message;
                if (!issue.Location.Path.empty())
                {
                    summary.FirstError += " at " + issue.Location.Path;
                }
                if (!issue.PartUri.empty())
                {
                    summary.FirstError += " in " + issue.PartUri;
                }
            }
            ++summary.ErrorCount;
        }
        else if (issue.Severity == ExyokiOffice::ValidationSeverity::Warning)
        {
            ++summary.WarningCount;
        }
    }

    summary.HasErrors = result.HasErrors();
    return summary;
}

PreservationSummary CheckPreservation(std::span<const ExyokiOffice::Byte> packageBytes)
{
    PreservationSummary summary;

    const ScopedTemporaryFile first("ExyokiOffice_preserve_a", ".zip");
    const ScopedTemporaryFile second("ExyokiOffice_preserve_b", ".zip");

    if (!TestSupportHelpers::WriteBytes(first.Path(), packageBytes))
    {
        return summary;
    }

    ExyokiOffice::OpenXmlPackage package;
    if (!package.LoadFromFile(first.Path()))
    {
        return summary;
    }
    if (!package.SaveToFile(second.Path()))
    {
        return summary;
    }

    const auto diff = ExyokiOffice::Tools::Compare(first.Path(), second.Path());
    if (!diff.Ok)
    {
        for (const auto& diagnostic : diff.Diagnostics)
        {
            summary.Differences.push_back(diagnostic.Message);
        }
        return summary;
    }

    summary.Ok = true;
    for (const auto& change : diff.PartChanges)
    {
        summary.Differences.push_back(TestSupportHelpers::DescribePartChange(change));
    }
    for (const auto& change : diff.RelationshipChanges)
    {
        summary.Differences.push_back(TestSupportHelpers::DescribeRelationshipChange(change));
    }

    summary.Preserved = summary.Differences.empty();
    return summary;
}

PreservationSummary ComparePackages(std::span<const ExyokiOffice::Byte> left,
                                    std::span<const ExyokiOffice::Byte> right)
{
    PreservationSummary summary;

    const ScopedTemporaryFile leftFile("ExyokiOffice_compare_a", ".zip");
    const ScopedTemporaryFile rightFile("ExyokiOffice_compare_b", ".zip");

    if (!TestSupportHelpers::WriteBytes(leftFile.Path(), left) ||
        !TestSupportHelpers::WriteBytes(rightFile.Path(), right))
    {
        return summary;
    }

    const auto diff = ExyokiOffice::Tools::Compare(leftFile.Path(), rightFile.Path());
    if (!diff.Ok)
    {
        for (const auto& diagnostic : diff.Diagnostics)
        {
            summary.Differences.push_back(diagnostic.Message);
        }
        return summary;
    }

    summary.Ok = true;
    for (const auto& change : diff.PartChanges)
    {
        summary.Differences.push_back(TestSupportHelpers::DescribePartChange(change));
    }
    for (const auto& change : diff.RelationshipChanges)
    {
        summary.Differences.push_back(TestSupportHelpers::DescribeRelationshipChange(change));
    }

    summary.Preserved = summary.Differences.empty();
    return summary;
}

std::shared_ptr<ExyokiOffice::Word::WordDocumentEditor>
RoundTrip(const std::shared_ptr<ExyokiOffice::Word::WordDocumentEditor>& editor)
{
    if (!editor)
    {
        return nullptr;
    }

    const auto bytes = editor->SaveToMemory();
    if (bytes.empty())
    {
        return nullptr;
    }

    return ExyokiOffice::Word::WordDocumentEditor::Open(bytes);
}

std::shared_ptr<ExyokiOffice::Excel::ExcelDocumentEditor>
RoundTrip(const std::shared_ptr<ExyokiOffice::Excel::ExcelDocumentEditor>& editor)
{
    if (!editor)
    {
        return nullptr;
    }

    const auto bytes = editor->SaveToMemory();
    if (bytes.empty())
    {
        return nullptr;
    }

    return ExyokiOffice::Excel::ExcelDocumentEditor::Open(bytes);
}

std::shared_ptr<ExyokiOffice::PowerPoint::PowerPointDocumentEditor>
RoundTrip(const std::shared_ptr<ExyokiOffice::PowerPoint::PowerPointDocumentEditor>& editor)
{
    if (!editor)
    {
        return nullptr;
    }

    const auto bytes = editor->SaveToMemory();
    if (bytes.empty())
    {
        return nullptr;
    }

    return ExyokiOffice::PowerPoint::PowerPointDocumentEditor::Open(bytes);
}

} // namespace ExyokiOfficeTests
