// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/OpenXmlPackageValidator.hpp"
#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"
#include "ExyokiOffice/Security/ExternalResources.hpp"
#include "ExyokiOffice/Tools/ExternalResourceInspector.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace
{
using ExyokiOffice::ICancellationToken;
using ExyokiOffice::OpenXmlPackage;
using ExyokiOffice::ValidationErrorId;
using ExyokiOffice::Security::CheckExternalReference;
using ExyokiOffice::Security::CollectExternalReferences;
using ExyokiOffice::Security::ExternalReference;
using ExyokiOffice::Security::ExternalResourceKind;
using ExyokiOffice::Security::ExternalResourcePolicy;
using ExyokiOffice::Security::ExternalResourceRequest;
using ExyokiOffice::Security::ExternalResourceResponse;
using ExyokiOffice::Security::ExternalResourceStatus;
using ExyokiOffice::Security::IExternalResourceResolver;
using ExyokiOffice::Security::ResolveExternalResource;

constexpr std::string_view kImageRelationship =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/image";
constexpr std::string_view kHyperlinkRelationship =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink";
constexpr std::string_view kAttachedTemplateRelationship =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/attachedTemplate";
constexpr std::string_view kExternalLinkPathRelationship =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/externalLinkPath";

/// Stands in for an application supplied resolver.
///
/// It reaches nothing: the payload is whatever the test put there, and every
/// call is recorded so a test can assert that the library did or did not ask.
class StubResolver final : public IExternalResourceResolver
{
public:
    ExternalResourceResponse Resolve(const ExternalResourceRequest& request) override
    {
        ++Calls;
        LastRequest = request;

        if (Throws)
        {
            throw std::runtime_error("resolver failure");
        }

        ExternalResourceResponse response;
        response.Status = Status;
        response.Data = Payload;
        response.ContentType = ContentType;
        response.EffectiveUri = EffectiveUri;
        return response;
    }

    int Calls = 0;
    ExternalResourceRequest LastRequest;
    ExternalResourceStatus Status = ExternalResourceStatus::Ok;
    std::vector<ExyokiOffice::Byte> Payload{1, 2, 3, 4};
    std::string ContentType;
    std::string EffectiveUri;
    bool Throws = false;
};

/// Cancellation flag a test can flip before or after the resolver runs.
class CancellationFlag final : public ICancellationToken
{
public:
    bool IsCancelled() const override { return Cancelled; }

    bool Cancelled = false;
};

/// A policy that allows exactly one https host for linked images.
ExternalResourcePolicy ImagePolicy()
{
    return ExternalResourcePolicy::HttpsOnly({"cdn.example.test"}, {ExternalResourceKind::LinkedImage});
}

/// A bare package carrying a single external relationship on its root.
std::shared_ptr<OpenXmlPackage> MakePackageWithTarget(std::string target, std::string_view type = kImageRelationship)
{
    auto package = std::make_shared<OpenXmlPackage>();
    const auto id = package->AddExternalRelationship(type, std::move(target));
    REQUIRE(!id.empty());
    return package;
}

ExternalReference OnlyReference(const OpenXmlPackage& package)
{
    const auto references = CollectExternalReferences(package);
    REQUIRE(references.size() == 1);
    return references.front();
}

bool HasIssue(const ExyokiOffice::ValidationResult& result, ValidationErrorId id)
{
    const auto& issues = result.Issues();
    return std::any_of(issues.begin(), issues.end(), [id](const auto& issue)
                       { return issue.Id == id; });
}

/// Smallest PNG the image format detector accepts.
std::vector<ExyokiOffice::Byte> PngBytes()
{
    return {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R',
            0, 0, 0, 16, 0, 0, 0, 16};
}
} // namespace

TEST_SUITE("ExternalResourceTests")
{
    TEST_CASE("a package resolves nothing until a resolver is installed [unit] [security] [external] [external-resources]")
    {
        auto package = MakePackageWithTarget("https://cdn.example.test/logo.png");
        // The point of the whole feature: the default state reaches nothing.
        CHECK(package->GetExternalResourceResolver() == nullptr);
        CHECK(package->GetExternalResourcePolicy().AllowedSchemes.empty());
        CHECK(package->GetExternalResourcePolicy().AllowedKinds.empty());

        const auto response = ResolveExternalResource(*package, OnlyReference(*package));
        CHECK(response.Status == ExternalResourceStatus::NoResolver);
        CHECK(response.Data.empty());
        CHECK(HasIssue(package->LastValidationResult(), ValidationErrorId::ExternalResourceNoResolver));
    }

    TEST_CASE("a resolver alone changes nothing while the policy denies everything [unit] [security] [external] [external-resources]")
    {
        auto resolver = std::make_shared<StubResolver>();
        auto package = MakePackageWithTarget("https://cdn.example.test/logo.png");
        package->SetExternalResourceResolver(resolver);

        const auto response = ResolveExternalResource(*package, OnlyReference(*package));
        CHECK(response.Status == ExternalResourceStatus::AccessDenied);
        CHECK(resolver->Calls == 0);
        CHECK(HasIssue(package->LastValidationResult(), ValidationErrorId::ExternalResourceDenied));
    }

    TEST_CASE("an allowed target reaches the resolver and returns its payload [unit] [security] [external] [external-resources]")
    {
        auto resolver = std::make_shared<StubResolver>();
        auto package = MakePackageWithTarget("https://cdn.example.test/logo.png");
        package->SetExternalResourceResolver(resolver);
        package->SetExternalResourcePolicy(ImagePolicy());

        const auto response = ResolveExternalResource(*package, OnlyReference(*package));
        REQUIRE(response.Succeeded());
        CHECK(response.Data == std::vector<ExyokiOffice::Byte>{1, 2, 3, 4});
        CHECK(resolver->Calls == 1);
        CHECK(package->LastValidationResult().Issues().empty());
    }

    TEST_CASE("the allowlist rejects other hosts, schemes, ports, and kinds [unit] [security] [external] [external-resources]")
    {
        auto resolver = std::make_shared<StubResolver>();

        SUBCASE("another host")
        {
            auto package = MakePackageWithTarget("https://evil.example.org/logo.png");
            package->SetExternalResourceResolver(resolver);
            package->SetExternalResourcePolicy(ImagePolicy());
            CHECK(ResolveExternalResource(*package, OnlyReference(*package)).Status ==
                  ExternalResourceStatus::AccessDenied);
        }

        SUBCASE("another scheme")
        {
            auto package = MakePackageWithTarget("http://cdn.example.test/logo.png");
            package->SetExternalResourceResolver(resolver);
            package->SetExternalResourcePolicy(ImagePolicy());
            CHECK(ResolveExternalResource(*package, OnlyReference(*package)).Status ==
                  ExternalResourceStatus::AccessDenied);
        }

        SUBCASE("another port on an allowed host")
        {
            auto package = MakePackageWithTarget("https://cdn.example.test:8443/logo.png");
            package->SetExternalResourceResolver(resolver);
            package->SetExternalResourcePolicy(ImagePolicy());
            CHECK(ResolveExternalResource(*package, OnlyReference(*package)).Status ==
                  ExternalResourceStatus::AccessDenied);
        }

        SUBCASE("a kind the policy does not list")
        {
            auto package = MakePackageWithTarget("https://cdn.example.test/page.html", kHyperlinkRelationship);
            package->SetExternalResourceResolver(resolver);
            package->SetExternalResourcePolicy(ImagePolicy());
            CHECK(ResolveExternalResource(*package, OnlyReference(*package)).Status ==
                  ExternalResourceStatus::AccessDenied);
        }

        CHECK(resolver->Calls == 0);
    }

    TEST_CASE("a subdomain entry matches the domain and everything under it [unit] [security] [external] [external-resources]")
    {
        auto resolver = std::make_shared<StubResolver>();
        auto policy = ExternalResourcePolicy::HttpsOnly({".example.test"}, {ExternalResourceKind::LinkedImage});

        for (const std::string host : {"example.test", "cdn.example.test", "a.b.example.test"})
        {
            auto package = MakePackageWithTarget("https://" + host + "/logo.png");
            package->SetExternalResourceResolver(resolver);
            package->SetExternalResourcePolicy(policy);
            CHECK(ResolveExternalResource(*package, OnlyReference(*package)).Succeeded());
        }

        auto other = MakePackageWithTarget("https://notexample.test/logo.png");
        other->SetExternalResourceResolver(resolver);
        other->SetExternalResourcePolicy(policy);
        CHECK(ResolveExternalResource(*other, OnlyReference(*other)).Status == ExternalResourceStatus::AccessDenied);
    }

    TEST_CASE("file targets honour the path prefix and refuse traversal [unit] [security] [external] [external-resources]")
    {
        auto resolver = std::make_shared<StubResolver>();
        ExternalResourcePolicy policy;
        policy.AllowedSchemes = {"file"};
        policy.AllowedPathPrefixes = {"/C:/assets"};
        policy.AllowedKinds = {ExternalResourceKind::LinkedImage};

        SUBCASE("inside the prefix")
        {
            auto package = MakePackageWithTarget("file:///C:/assets/logo.png");
            package->SetExternalResourceResolver(resolver);
            package->SetExternalResourcePolicy(policy);
            CHECK(ResolveExternalResource(*package, OnlyReference(*package)).Succeeded());
            CHECK(resolver->Calls == 1);
        }

        SUBCASE("a sibling directory that merely shares the prefix text")
        {
            auto package = MakePackageWithTarget("file:///C:/assets-private/logo.png");
            package->SetExternalResourceResolver(resolver);
            package->SetExternalResourcePolicy(policy);
            CHECK(ResolveExternalResource(*package, OnlyReference(*package)).Status ==
                  ExternalResourceStatus::AccessDenied);
            CHECK(resolver->Calls == 0);
        }

        SUBCASE("traversal out of the prefix")
        {
            auto package = MakePackageWithTarget("file:///C:/assets/../secrets/key.png");
            package->SetExternalResourceResolver(resolver);
            package->SetExternalResourcePolicy(policy);
            CHECK(ResolveExternalResource(*package, OnlyReference(*package)).Status ==
                  ExternalResourceStatus::AccessDenied);
            CHECK(resolver->Calls == 0);
        }

        SUBCASE("traversal hidden behind percent encoding")
        {
            auto package = MakePackageWithTarget("file:///C:/assets/%2e%2e/secrets/key.png");
            package->SetExternalResourceResolver(resolver);
            package->SetExternalResourcePolicy(policy);
            CHECK(ResolveExternalResource(*package, OnlyReference(*package)).Status ==
                  ExternalResourceStatus::AccessDenied);
            CHECK(resolver->Calls == 0);
        }

        SUBCASE("a UNC share is treated as a file target")
        {
            auto package = MakePackageWithTarget("\\\\server\\assets\\logo.png");
            package->SetExternalResourceResolver(resolver);
            package->SetExternalResourcePolicy(policy);
            // The host is not in the allowlist, so the share stays unreachable.
            CHECK(ResolveExternalResource(*package, OnlyReference(*package)).Status ==
                  ExternalResourceStatus::AccessDenied);
            CHECK(resolver->Calls == 0);
        }
    }

    TEST_CASE("credentials in a URI are refused unless allowed [unit] [security] [external] [external-resources]")
    {
        auto resolver = std::make_shared<StubResolver>();
        auto package = MakePackageWithTarget("https://user:secret@cdn.example.test/logo.png");
        package->SetExternalResourceResolver(resolver);
        package->SetExternalResourcePolicy(ImagePolicy());

        CHECK(ResolveExternalResource(*package, OnlyReference(*package)).Status == ExternalResourceStatus::AccessDenied);
        CHECK(resolver->Calls == 0);

        auto permissive = ImagePolicy();
        permissive.AllowUserInfoInUri = true;
        package->SetExternalResourcePolicy(permissive);
        CHECK(ResolveExternalResource(*package, OnlyReference(*package)).Succeeded());
        CHECK(resolver->Calls == 1);
    }

    TEST_CASE("a relative target needs a base URI [unit] [security] [external] [external-resources]")
    {
        auto resolver = std::make_shared<StubResolver>();
        auto package = MakePackageWithTarget("assets/logo.png");
        package->SetExternalResourceResolver(resolver);
        package->SetExternalResourcePolicy(ImagePolicy());

        CHECK(ResolveExternalResource(*package, OnlyReference(*package)).Status == ExternalResourceStatus::AccessDenied);
        CHECK(resolver->Calls == 0);

        auto policy = ImagePolicy();
        policy.BaseUri = "https://cdn.example.test/site/index.html";
        package->SetExternalResourcePolicy(policy);
        REQUIRE(ResolveExternalResource(*package, OnlyReference(*package)).Succeeded());
        CHECK(resolver->LastRequest.Uri == "https://cdn.example.test/site/assets/logo.png");
        CHECK(resolver->LastRequest.OriginalTarget == "assets/logo.png");
    }

    TEST_CASE("the request carries the policy limits and the relationship identity [unit] [security] [external] [external-resources]")
    {
        auto resolver = std::make_shared<StubResolver>();
        auto editor = ExyokiOffice::Word::WordDocumentEditor::CreateNew();
        REQUIRE(editor);
        auto document = editor->GetDocument();
        auto mainPart = document->GetMainDocumentPart();
        REQUIRE(mainPart);
        const auto relationshipId =
            mainPart->AddExternalRelationship(kImageRelationship, "https://cdn.example.test/logo.png");
        REQUIRE(!relationshipId.empty());

        auto policy = ImagePolicy();
        policy.Timeout = std::chrono::milliseconds(1234);
        policy.MaxResourceBytes = 4096;
        document->SetExternalResourceResolver(resolver);
        document->SetExternalResourcePolicy(policy);

        CancellationFlag token;
        REQUIRE(ResolveExternalResource(*document, OnlyReference(*document), &token).Succeeded());
        CHECK(resolver->LastRequest.Timeout == std::chrono::milliseconds(1234));
        CHECK(resolver->LastRequest.MaxBytes == 4096);
        CHECK(resolver->LastRequest.SourcePartUri == mainPart->Uri());
        CHECK(resolver->LastRequest.RelationshipId == relationshipId);
        CHECK(resolver->LastRequest.RelationshipType == kImageRelationship);
        CHECK(resolver->LastRequest.Kind == ExternalResourceKind::LinkedImage);
        CHECK(resolver->LastRequest.CancellationToken == &token);
    }

    TEST_CASE("an oversized response is discarded [unit] [security] [external] [external-resources]")
    {
        auto resolver = std::make_shared<StubResolver>();
        resolver->Payload.assign(64, 0x7F);

        auto policy = ImagePolicy();
        policy.MaxResourceBytes = 16;
        auto package = MakePackageWithTarget("https://cdn.example.test/logo.png");
        package->SetExternalResourceResolver(resolver);
        package->SetExternalResourcePolicy(policy);

        const auto response = ResolveExternalResource(*package, OnlyReference(*package));
        CHECK(response.Status == ExternalResourceStatus::TooLarge);
        CHECK(response.Data.empty());
        CHECK(HasIssue(package->LastValidationResult(), ValidationErrorId::ExternalResourceTooLarge));
    }

    TEST_CASE("the per-package budgets stop repeated requests [unit] [security] [external] [external-resources]")
    {
        SUBCASE("request count")
        {
            auto resolver = std::make_shared<StubResolver>();
            auto policy = ImagePolicy();
            policy.MaxRequests = 2;
            auto package = MakePackageWithTarget("https://cdn.example.test/logo.png");
            package->SetExternalResourceResolver(resolver);
            package->SetExternalResourcePolicy(policy);
            const auto reference = OnlyReference(*package);

            CHECK(ResolveExternalResource(*package, reference).Succeeded());
            CHECK(ResolveExternalResource(*package, reference).Succeeded());
            CHECK(ResolveExternalResource(*package, reference).Status == ExternalResourceStatus::BudgetExceeded);
            CHECK(resolver->Calls == 2);

            package->ResetExternalResourceBudget();
            CHECK(ResolveExternalResource(*package, reference).Succeeded());
        }

        SUBCASE("total bytes")
        {
            auto resolver = std::make_shared<StubResolver>();
            resolver->Payload.assign(8, 0x01);
            auto policy = ImagePolicy();
            policy.MaxTotalBytes = 10;
            auto package = MakePackageWithTarget("https://cdn.example.test/logo.png");
            package->SetExternalResourceResolver(resolver);
            package->SetExternalResourcePolicy(policy);
            const auto reference = OnlyReference(*package);

            CHECK(ResolveExternalResource(*package, reference).Succeeded());
            // Only two bytes of budget are left, so the same payload no longer fits.
            const auto second = ResolveExternalResource(*package, reference);
            CHECK(second.Status == ExternalResourceStatus::TooLarge);
            CHECK(resolver->LastRequest.MaxBytes == 2);
        }
    }

    TEST_CASE("a redirect out of the allowlist drops the data [unit] [security] [external] [external-resources]")
    {
        auto resolver = std::make_shared<StubResolver>();
        resolver->EffectiveUri = "https://evil.example.org/logo.png";
        auto package = MakePackageWithTarget("https://cdn.example.test/logo.png");
        package->SetExternalResourceResolver(resolver);
        package->SetExternalResourcePolicy(ImagePolicy());

        const auto response = ResolveExternalResource(*package, OnlyReference(*package));
        CHECK(response.Status == ExternalResourceStatus::AccessDenied);
        CHECK(response.Data.empty());
        CHECK(HasIssue(package->LastValidationResult(), ValidationErrorId::ExternalResourceDenied));
    }

    TEST_CASE("a failing or throwing resolver is reported, not propagated [unit] [security] [external] [external-resources]")
    {
        SUBCASE("a reported failure")
        {
            auto resolver = std::make_shared<StubResolver>();
            resolver->Status = ExternalResourceStatus::NotFound;
            auto package = MakePackageWithTarget("https://cdn.example.test/logo.png");
            package->SetExternalResourceResolver(resolver);
            package->SetExternalResourcePolicy(ImagePolicy());

            const auto response = ResolveExternalResource(*package, OnlyReference(*package));
            CHECK(response.Status == ExternalResourceStatus::NotFound);
            CHECK(response.Data.empty());
            CHECK(HasIssue(package->LastValidationResult(), ValidationErrorId::ExternalResourceUnavailable));
        }

        SUBCASE("an exception escaping the resolver")
        {
            auto resolver = std::make_shared<StubResolver>();
            resolver->Throws = true;
            auto package = MakePackageWithTarget("https://cdn.example.test/logo.png");
            package->SetExternalResourceResolver(resolver);
            package->SetExternalResourcePolicy(ImagePolicy());

            const auto response = ResolveExternalResource(*package, OnlyReference(*package));
            CHECK(response.Status == ExternalResourceStatus::Failed);
            CHECK(HasIssue(package->LastValidationResult(), ValidationErrorId::ExternalResourceUnavailable));
        }
    }

    TEST_CASE("cancellation discards a response that already arrived [unit] [security] [external] [external-resources]")
    {
        auto resolver = std::make_shared<StubResolver>();
        auto package = MakePackageWithTarget("https://cdn.example.test/logo.png");
        package->SetExternalResourceResolver(resolver);
        package->SetExternalResourcePolicy(ImagePolicy());

        CancellationFlag token;
        token.Cancelled = true;
        const auto response = ResolveExternalResource(*package, OnlyReference(*package), &token);
        CHECK(response.Status == ExternalResourceStatus::Cancelled);
        CHECK(response.Data.empty());
    }

    TEST_CASE("a policy check consumes no budget and never asks the resolver [unit] [security] [external] [external-resources]")
    {
        auto resolver = std::make_shared<StubResolver>();
        auto package = MakePackageWithTarget("https://cdn.example.test/page.html", kHyperlinkRelationship);
        package->SetExternalResourceResolver(resolver);
        package->SetExternalResourcePolicy(
            ExternalResourcePolicy::HttpsOnly({"cdn.example.test"}, {ExternalResourceKind::Hyperlink}));

        const auto reference = OnlyReference(*package);
        for (int index = 0; index < 200; ++index)
        {
            CHECK(CheckExternalReference(*package, reference) == ExternalResourceStatus::Ok);
        }
        CHECK(resolver->Calls == 0);
    }

    TEST_CASE("a policy check works without any resolver at all [unit] [security] [external] [external-resources]")
    {
        auto package = MakePackageWithTarget("https://evil.example.org/page.html", kHyperlinkRelationship);
        package->SetExternalResourcePolicy(
            ExternalResourcePolicy::HttpsOnly({"cdn.example.test"}, {ExternalResourceKind::Hyperlink}));
        CHECK(CheckExternalReference(*package, OnlyReference(*package)) == ExternalResourceStatus::AccessDenied);
    }

    TEST_CASE("external references are enumerated and classified [unit] [security] [external] [external-resources]")
    {
        auto editor = ExyokiOffice::Word::WordDocumentEditor::CreateNew();
        REQUIRE(editor);
        auto document = editor->GetDocument();
        auto mainPart = document->GetMainDocumentPart();
        REQUIRE(mainPart);

        REQUIRE(!mainPart->AddExternalRelationship(kImageRelationship, "https://cdn.example.test/logo.png").empty());
        REQUIRE(!mainPart->AddExternalRelationship(kHyperlinkRelationship, "https://example.test/page").empty());
        REQUIRE(!document->AddExternalRelationship(kAttachedTemplateRelationship, "templates/report.dotx").empty());
        REQUIRE(!document->AddExternalRelationship(kExternalLinkPathRelationship, "books/prices.xlsx").empty());

        const auto references = CollectExternalReferences(*document);
        REQUIRE(references.size() == 4);

        const auto kindOf = [&references](std::string_view target)
        {
            const auto it = std::find_if(references.begin(),
                                         references.end(),
                                         [target](const auto& reference)
                                         { return reference.Target == target; });
            REQUIRE(it != references.end());
            return it->Kind;
        };

        CHECK(kindOf("https://cdn.example.test/logo.png") == ExternalResourceKind::LinkedImage);
        CHECK(kindOf("https://example.test/page") == ExternalResourceKind::Hyperlink);
        CHECK(kindOf("templates/report.dotx") == ExternalResourceKind::AttachedTemplate);
        CHECK(kindOf("books/prices.xlsx") == ExternalResourceKind::ExternalWorkbook);

        // The package root sorts before every part.
        CHECK(references.front().SourcePartUri == "/");
    }

    TEST_CASE("the Tools inspector lists references without reaching any of them [unit] [tools] [external] [external-resources]")
    {
        auto editor = ExyokiOffice::Word::WordDocumentEditor::CreateNew();
        REQUIRE(editor);
        auto mainPart = editor->GetDocument()->GetMainDocumentPart();
        REQUIRE(mainPart);
        REQUIRE(!mainPart->AddExternalRelationship(kImageRelationship, "https://cdn.example.test/logo.png").empty());

        const auto report = ExyokiOffice::Tools::InspectExternalResources(*editor->GetDocument());
        REQUIRE(report.Loaded);
        REQUIRE(report.Count() == 1);
        CHECK(report.References.front().Kind == "LinkedImage");
        CHECK(report.References.front().Target == "https://cdn.example.test/logo.png");
        CHECK(report.References.front().SourcePartUri == mainPart->Uri());
    }

    TEST_CASE("loading, saving and validating never call a resolver [unit] [security] [external] [external-resources]")
    {
        auto resolver = std::make_shared<StubResolver>();

        auto editor = ExyokiOffice::Word::WordDocumentEditor::CreateNew();
        REQUIRE(editor);
        editor->AddParagraph("Text with an outward link");
        auto mainPart = editor->GetDocument()->GetMainDocumentPart();
        REQUIRE(mainPart);
        REQUIRE(!mainPart->AddExternalRelationship(kImageRelationship, "https://cdn.example.test/logo.png").empty());
        REQUIRE(!mainPart->AddExternalRelationship(kHyperlinkRelationship, "https://example.test/page").empty());

        ExyokiOffice::Packaging::OpenSettings settings;
        settings.ExternalResources = resolver;
        // Even the most permissive policy must not make opening reach outward.
        settings.ExternalResourcePolicy = ExternalResourcePolicy::HttpsOnly(
            {".example.test"}, {ExternalResourceKind::LinkedImage, ExternalResourceKind::Hyperlink});

        const auto bytes = editor->SaveToMemory();
        REQUIRE(!bytes.empty());

        auto reopened = ExyokiOffice::Word::WordDocumentEditor::Open(bytes, settings);
        REQUIRE(reopened);
        const auto savedAgain = reopened->SaveToMemory();
        REQUIRE(!savedAgain.empty());

        ExyokiOffice::OpenXmlPackageValidator validator;
        const auto validation = validator.Validate(*reopened->GetDocument());
        (void)validation;

        CHECK(resolver->Calls == 0);
        // The settings did install both, so the failure to call is a decision,
        // not an accident of the resolver never having been attached.
        CHECK(reopened->GetDocument()->GetExternalResourceResolver() == resolver);
        CHECK(reopened->GetDocument()->GetExternalResourcePolicy().AllowedSchemes.size() == 1);
    }

    TEST_CASE("a linked PowerPoint picture can be embedded through the resolver [unit] [powerpoint] [external] [external-resources]")
    {
        using namespace ExyokiOffice::PowerPoint;

        auto resolver = std::make_shared<StubResolver>();
        resolver->Payload = PngBytes();

        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor);
        auto slide = editor->AddSlide();
        REQUIRE(slide);

        PresentationPictureData linked;
        linked.LinkedUri = "https://cdn.example.test/logo.png";
        linked.Transform.Size = {100, 200};
        auto picture = slide->ShapeTree()->AddPicture(linked);
        REQUIRE(picture);
        REQUIRE(slide->GetPart()->GetImageParts().empty());

        auto document = editor->GetDocument();
        document->SetExternalResourceResolver(resolver);
        document->SetExternalResourcePolicy(ImagePolicy());

        REQUIRE(picture->EmbedLinkedPicture() == ExternalResourceStatus::Ok);
        CHECK(resolver->Calls == 1);

        const auto stored = picture->GetPicture();
        REQUIRE(stored);
        CHECK_FALSE(stored->LinkedUri.has_value());
        REQUIRE(stored->Embedded.has_value());
        CHECK(stored->Embedded->Data == PngBytes());
        CHECK(stored->Embedded->ContentType == "image/png");
        CHECK(slide->GetPart()->GetImageParts().size() == 1);
        CHECK(CollectExternalReferences(*document).empty());
    }

    TEST_CASE("embedding a linked picture leaves it alone when the policy refuses [unit] [powerpoint] [external] [external-resources]")
    {
        using namespace ExyokiOffice::PowerPoint;

        auto resolver = std::make_shared<StubResolver>();
        resolver->Payload = PngBytes();

        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor);
        auto slide = editor->AddSlide();
        PresentationPictureData linked;
        linked.LinkedUri = "https://evil.example.org/logo.png";
        linked.Transform.Size = {100, 200};
        auto picture = slide->ShapeTree()->AddPicture(linked);
        REQUIRE(picture);

        auto document = editor->GetDocument();
        document->SetExternalResourceResolver(resolver);
        document->SetExternalResourcePolicy(ImagePolicy());

        CHECK(picture->EmbedLinkedPicture() == ExternalResourceStatus::AccessDenied);
        CHECK(resolver->Calls == 0);
        REQUIRE(picture->GetPicture());
        CHECK(picture->GetPicture()->LinkedUri == linked.LinkedUri);
        CHECK(slide->GetPart()->GetImageParts().empty());
    }

    TEST_CASE("an attached template is only readable through a resolver [unit] [word] [external] [external-resources]")
    {
        auto resolver = std::make_shared<StubResolver>();
        resolver->Payload.assign(32, 0x50);

        auto editor = ExyokiOffice::Word::WordDocumentEditor::CreateNew();
        REQUIRE(editor);
        auto document = editor->GetDocument();
        CHECK(document->ResolveAttachedTemplate().Status == ExternalResourceStatus::NotFound);

        auto settingsPart = document->EnsureDocumentSettingsPart();
        REQUIRE(settingsPart);
        REQUIRE(!settingsPart->AddExternalRelationship(kAttachedTemplateRelationship, "report.dotx").empty());
        REQUIRE(document->GetAttachedTemplateReference().has_value());

        CHECK(document->ResolveAttachedTemplate().Status == ExternalResourceStatus::NoResolver);

        auto policy = ExternalResourcePolicy::HttpsOnly({"templates.example.test"},
                                                        {ExternalResourceKind::AttachedTemplate});
        policy.BaseUri = "https://templates.example.test/word/base.dotx";
        document->SetExternalResourceResolver(resolver);
        document->SetExternalResourcePolicy(policy);

        const auto response = document->ResolveAttachedTemplate();
        REQUIRE(response.Succeeded());
        CHECK(response.Data.size() == 32);
        CHECK(resolver->LastRequest.Uri == "https://templates.example.test/word/report.dotx");
        CHECK(resolver->LastRequest.Kind == ExternalResourceKind::AttachedTemplate);
    }
}
