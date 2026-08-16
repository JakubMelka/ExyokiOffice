// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "TestSupport.hpp"

#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

using ExyokiOffice::Packaging::OpenErrorCode;
using ExyokiOffice::Word::WordDocumentEditor;

class OpenErrorTestHelpers
{
public:
    using Bytes = std::vector<ExyokiOffice::Byte>;

    static Bytes WordPackageBytes()
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor);
        editor->AddParagraph("Content");
        auto bytes = editor->SaveToMemory();
        REQUIRE(!bytes.empty());
        return bytes;
    }

    static Bytes PresentationPackageBytes()
    {
        auto editor = ExyokiOffice::PowerPoint::PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor);
        auto bytes = editor->SaveToMemory();
        REQUIRE(!bytes.empty());
        return bytes;
    }

    static std::filesystem::path WriteFile(std::string_view extension, const Bytes& bytes)
    {
        const auto path = ExyokiOfficeTests::MakeTemporaryPath("exyoki_open_error", extension);
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        REQUIRE(stream.is_open());
        stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        stream.close();
        return path;
    }
};

TEST_SUITE("Package.OpenError")
{
    TEST_CASE("A missing file is reported as a missing file [unit] [open-error]")
    {
        // Every one of these used to be the same nullptr, which is why a service
        // could not tell its user whether the upload was corrupt, encrypted, or
        // simply too large.
        ExyokiOffice::Packaging::OpenError error;
        const auto path = ExyokiOfficeTests::MakeTemporaryPath("exyoki_open_missing", ".docx");
        CHECK(WordDocumentEditor::Open(path, {}, nullptr, &error) == nullptr);
        CHECK(error.Code == OpenErrorCode::FileNotFound);
        CHECK_FALSE(error.Message.empty());
        CHECK(static_cast<bool>(error));
    }

    TEST_CASE("Bytes that are not a package say so [unit] [open-error]")
    {
        ExyokiOffice::Packaging::OpenError error;
        const OpenErrorTestHelpers::Bytes notAZip{'h', 'e', 'l', 'l', 'o'};
        CHECK(WordDocumentEditor::Open(notAZip, {}, nullptr, &error) == nullptr);
        CHECK(error.Code == OpenErrorCode::NotAPackage);
    }

    TEST_CASE("An empty argument is told apart from a broken package [unit] [open-error]")
    {
        ExyokiOffice::Packaging::OpenError error;
        CHECK(WordDocumentEditor::Open(OpenErrorTestHelpers::Bytes{}, {}, nullptr, &error) == nullptr);
        CHECK(error.Code == OpenErrorCode::InvalidArgument);
    }

    TEST_CASE("Exceeding a configured limit is reported as such, with diagnostics [unit] [open-error]")
    {
        const auto bytes = OpenErrorTestHelpers::WordPackageBytes();

        ExyokiOffice::Packaging::OpenSettings settings;
        settings.PackageLimits = ExyokiOffice::OpenXmlPackageLimits::Recommended();
        settings.PackageLimits.MaxEntries = 1;

        ExyokiOffice::Packaging::OpenError error;
        CHECK(WordDocumentEditor::Open(bytes, settings, nullptr, &error) == nullptr);
        CHECK(error.Code == OpenErrorCode::LimitExceeded);
        // The package that knew the reason is destroyed on the way out; the
        // diagnostics have to be copied out before that or they are lost.
        CHECK_FALSE(error.Diagnostics.Issues().empty());
    }

    TEST_CASE("A document of the wrong family is not just 'not a package' [unit] [open-error]")
    {
        // A .docx handed to the PowerPoint API is readable, well-formed, and
        // still not a presentation.
        const auto bytes = OpenErrorTestHelpers::WordPackageBytes();
        ExyokiOffice::Packaging::OpenError error;
        CHECK(ExyokiOffice::PowerPoint::PowerPointDocumentEditor::Open(bytes, {}, nullptr, &error) == nullptr);
        CHECK(error.Code == OpenErrorCode::WrongDocumentType);
    }

    TEST_CASE("Word and Excel refuse a package whose main part is missing [unit] [open-error]")
    {
        // The generic OPC loader reads a .pptx without complaint, so nothing
        // before this point notices the family. Both editors used to hand back a
        // working object over a package with no document in it.
        const auto presentation = OpenErrorTestHelpers::PresentationPackageBytes();

        ExyokiOffice::Packaging::OpenError wordError;
        CHECK(WordDocumentEditor::Open(presentation, {}, nullptr, &wordError) == nullptr);
        CHECK(wordError.Code == OpenErrorCode::WrongDocumentType);

        ExyokiOffice::Packaging::OpenError excelError;
        CHECK(ExyokiOffice::Excel::ExcelDocumentEditor::Open(presentation, {}, nullptr, &excelError) == nullptr);
        CHECK(excelError.Code == OpenErrorCode::WrongDocumentType);

        // And the other way round, so that neither answer is an accident of the
        // package the test happened to build.
        const auto word = OpenErrorTestHelpers::WordPackageBytes();
        ExyokiOffice::Packaging::OpenError wordAsExcel;
        CHECK(ExyokiOffice::Excel::ExcelDocumentEditor::Open(word, {}, nullptr, &wordAsExcel) == nullptr);
        CHECK(wordAsExcel.Code == OpenErrorCode::WrongDocumentType);
    }

    TEST_CASE("PowerPoint enforces the character budget it documents [unit] [open-error]")
    {
        // FinishOpen used to run the markup compatibility pass and nothing else,
        // so a caller who opened an untrusted .pptx under a character budget did
        // not get one. (The validation half of the same fix is pinned down in
        // OpcValidationTests, where a package can be given a defect on purpose.)
        const auto bytes = OpenErrorTestHelpers::PresentationPackageBytes();

        ExyokiOffice::Packaging::OpenSettings settings;
        settings.MaxCharactersInPart = 32;

        ExyokiOffice::Packaging::OpenError error;
        CHECK(ExyokiOffice::PowerPoint::PowerPointDocumentEditor::Open(bytes, settings, nullptr, &error) == nullptr);
        CHECK(error.Code == OpenErrorCode::PartTooLarge);

        // And the same package opens when nothing is asked of it.
        CHECK(ExyokiOffice::PowerPoint::PowerPointDocumentEditor::Open(bytes) != nullptr);
    }

    TEST_CASE("A file that cannot be opened is not called a broken package [unit] [open-error]")
    {
#ifdef _WIN32
        // The only portable way to make an existing file unreadable is to hold
        // it open with no sharing, which is a Windows call; elsewhere the code
        // path is the same but the setup is not, so the check runs here.
        const auto path = OpenErrorTestHelpers::WriteFile(".docx", OpenErrorTestHelpers::WordPackageBytes());
        void* handle = ::CreateFileW(path.wstring().c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(handle != INVALID_HANDLE_VALUE);

        ExyokiOffice::Packaging::OpenError error;
        CHECK(WordDocumentEditor::Open(path, {}, nullptr, &error) == nullptr);
        ::CloseHandle(handle);

        CHECK(error.Code == OpenErrorCode::FileUnreadable);
        CHECK_FALSE(error.Message.empty());
#endif
    }

    TEST_CASE("Cancelling during the load is reported as cancellation [unit] [open-error]")
    {
        // The token is only signalled once the loader is already inside the
        // package, so the load fails halfway and leaves behind exactly the
        // wreckage a corrupt file would. Blaming the document for that made the
        // one failure a caller causes on purpose the hardest to recognise.
        class CancelAfterFirstQuestion final : public ExyokiOffice::ICancellationToken
        {
        public:
            bool IsCancelled() const override
            {
                const bool cancelled = m_asked > 0;
                ++m_asked;
                return cancelled;
            }

        private:
            mutable int m_asked = 0;
        };

        const auto bytes = OpenErrorTestHelpers::WordPackageBytes();
        CancelAfterFirstQuestion token;
        ExyokiOffice::Packaging::OpenError error;
        CHECK(WordDocumentEditor::Open(bytes, {}, &token, &error) == nullptr);
        CHECK(error.Code == OpenErrorCode::Cancelled);
    }

    TEST_CASE("Passing no error object changes nothing [unit] [open-error]")
    {
        // The parameter is optional; a caller that does not want the reason must
        // not have to pass an object to be allowed to fail.
        const OpenErrorTestHelpers::Bytes notAZip{'n', 'o', 'p', 'e'};
        CHECK(WordDocumentEditor::Open(notAZip) == nullptr);
        CHECK(ExyokiOffice::Excel::ExcelDocumentEditor::Open(notAZip) == nullptr);
    }

    TEST_CASE("A successful open leaves the error untouched [unit] [open-error]")
    {
        const auto bytes = OpenErrorTestHelpers::WordPackageBytes();
        ExyokiOffice::Packaging::OpenError error;
        auto editor = WordDocumentEditor::Open(bytes, {}, nullptr, &error);
        REQUIRE(editor);
        CHECK(error.Code == OpenErrorCode::None);
        CHECK_FALSE(static_cast<bool>(error));
    }
}

TEST_SUITE("Package.DefaultLimits")
{
    TEST_CASE("A package nobody configured still refuses a decompression bomb [unit] [open-error]")
    {
        // The library used to start at Unlimited(), so an application that never
        // read the documentation had no defence at all. It now starts at
        // Recommended(), and asking for no limits is something a caller says.
        CHECK(ExyokiOffice::OpenXmlPackage::DefaultPackageLimits().MaxCompressionRatio ==
              ExyokiOffice::OpenXmlPackageLimits::Recommended().MaxCompressionRatio);
        CHECK(ExyokiOffice::OpenXmlPackage::DefaultPackageLimits().MaxXmlDepth ==
              ExyokiOffice::OpenXmlPackageLimits::Recommended().MaxXmlDepth);
        CHECK_FALSE(ExyokiOffice::OpenXmlPackage::ConfiguredDefaultPackageLimits().has_value());

        ExyokiOffice::OpenXmlPackage package;
        CHECK(package.PackageLimits().MaxUncompressedBytes ==
              ExyokiOffice::OpenXmlPackageLimits::Recommended().MaxUncompressedBytes);

        ExyokiOffice::Packaging::OpenSettings settings;
        CHECK(settings.PackageLimits.MaxEntries == ExyokiOffice::OpenXmlPackageLimits::Recommended().MaxEntries);
    }

    TEST_CASE("An explicit Unlimited() still means unlimited [unit] [open-error]")
    {
        ExyokiOffice::OpenXmlPackage package;
        package.SetPackageLimits(ExyokiOffice::OpenXmlPackageLimits::Unlimited());
        CHECK(package.PackageLimits().MaxEntries == 0);
        CHECK(package.PackageLimits().MaxXmlDepth == 0);
    }
}
