// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

// Helpers every test layer needs. Three of them used to be copy-pasted into a
// dozen test files each; the copies mattered once the suite was split into
// several CTest entries that run concurrently, because some of them used fixed
// temporary file names that collide across processes.
//
// Nothing here includes doctest. The helpers return values and the calling test
// asserts on them, so the support library stays independent of the assertion
// framework and of the order in which layers are linked.

#include "ExyokiOffice/StandardTypes.hpp"

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ExyokiOffice::Word
{
class WordDocumentEditor;
}

namespace ExyokiOffice::Excel
{
class ExcelDocumentEditor;
}

namespace ExyokiOffice::PowerPoint
{
class PowerPointDocumentEditor;
}

namespace ExyokiOfficeTests
{

/**
 * @brief Builds a unique path inside this process's temporary directory.
 *
 * Every path returned here lives under one per-process root beneath the system
 * temporary directory, and that root is removed when the test executable exits.
 * A case therefore does not have to clean up after itself: whatever it writes
 * to the returned path is gone once the run ends, including when the case was
 * cut short by a failing REQUIRE.
 *
 * The root name carries a steady-clock tick and the process id, so two test
 * executables started by `ctest -j` never collide even when they run the same
 * test case; the leaf adds a per-process counter. Set
 * `EXYOKIOFFICE_TESTS_KEEP_TEMP` to keep the root for inspection instead — the
 * path is printed on exit.
 *
 * The root is created on disk on the first call. The returned path itself is
 * not: it names a file or directory the caller is expected to create.
 */
[[nodiscard]] std::filesystem::path MakeTemporaryPath(std::string_view stem, std::string_view extension);

/**
 * @brief A temporary path that removes itself at the end of its scope.
 *
 * Cleanup at exit already covers every path MakeTemporaryPath hands out, so
 * this is for cases that need the removal to happen earlier — a loop building
 * large packages, or an assertion that the path is gone. It removes the file
 * (or directory tree) in the destructor and never throws.
 */
class ScopedTemporaryFile
{
public:
    ScopedTemporaryFile(std::string_view stem, std::string_view extension);
    ~ScopedTemporaryFile();

    ScopedTemporaryFile(const ScopedTemporaryFile&) = delete;
    ScopedTemporaryFile& operator=(const ScopedTemporaryFile&) = delete;
    ScopedTemporaryFile(ScopedTemporaryFile&&) = delete;
    ScopedTemporaryFile& operator=(ScopedTemporaryFile&&) = delete;

    [[nodiscard]] const std::filesystem::path& Path() const noexcept { return m_path; }

private:
    std::filesystem::path m_path;
};

/** Outcome of running OpenXmlPackageValidator over a saved package. */
struct ValidationSummary
{
    /** False when the bytes could not be loaded as an OPC package at all. */
    bool Loaded = false;
    bool HasErrors = false;
    ExyokiOffice::Size ErrorCount = 0;
    ExyokiOffice::Size WarningCount = 0;
    /** Message, element name and XPath of the first error, for CAPTURE. */
    std::string FirstError;
};

/** Loads @p packageBytes and validates it with the default target version. */
[[nodiscard]] ValidationSummary ValidatePackage(std::span<const ExyokiOffice::Byte> packageBytes);

/** Outcome of an open-save-compare cycle over a saved package. */
struct PreservationSummary
{
    /** False when a save, an open, or the comparison itself failed. */
    bool Ok = false;
    /** True when the second package is identical to the first. */
    bool Preserved = false;
    /** One human-readable line per difference, empty when Preserved. */
    std::vector<std::string> Differences;
};

/**
 * @brief Checks that a package survives open → save byte-for-byte.
 *
 * The bytes are written out, loaded through OpenXmlPackage, and saved again;
 * the two files are then compared part-by-part and relationship-by-relationship
 * with Tools::Compare. The package layer is used deliberately rather than a
 * family editor: it performs no save-time property update, so a difference in
 * the result is a real preservation failure and not a refreshed
 * `dcterms:modified`.
 */
[[nodiscard]] PreservationSummary CheckPreservation(std::span<const ExyokiOffice::Byte> packageBytes);

/**
 * @brief Compares two saved packages part-by-part and relationship-by-relationship.
 *
 * Use this instead of comparing the two byte buffers directly. A ZIP entry
 * carries a DOS timestamp taken from the wall clock when the entry is written
 * (`time(NULL)`, two-second resolution), so two saves of an unchanged package
 * differ in raw bytes whenever they straddle a tick — a test asserting
 * `after == before` on the buffers fails at random under load. The comparison
 * here covers what such a test actually means: added, removed, retyped, or
 * modified parts, and every relationship edge.
 *
 * PreservationSummary::Preserved is true when the two packages carry the same
 * content; Differences lists one human-readable line per change.
 */
[[nodiscard]] PreservationSummary ComparePackages(std::span<const ExyokiOffice::Byte> left,
                                                  std::span<const ExyokiOffice::Byte> right);

/** Saves the editor to memory and reopens it; returns nullptr when either step fails. */
[[nodiscard]] std::shared_ptr<ExyokiOffice::Word::WordDocumentEditor>
RoundTrip(const std::shared_ptr<ExyokiOffice::Word::WordDocumentEditor>& editor);

/** @copydoc RoundTrip */
[[nodiscard]] std::shared_ptr<ExyokiOffice::Excel::ExcelDocumentEditor>
RoundTrip(const std::shared_ptr<ExyokiOffice::Excel::ExcelDocumentEditor>& editor);

/** @copydoc RoundTrip */
[[nodiscard]] std::shared_ptr<ExyokiOffice::PowerPoint::PowerPointDocumentEditor>
RoundTrip(const std::shared_ptr<ExyokiOffice::PowerPoint::PowerPointDocumentEditor>& editor);

} // namespace ExyokiOfficeTests
