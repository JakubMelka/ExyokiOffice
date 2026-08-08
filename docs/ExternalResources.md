# External resources

An Office document can point outside itself: a picture referenced with
`a:blip r:link` instead of `r:embed`, a Word document attached to a template, a
workbook that links to another workbook, a hyperlink, a linked OLE object. In
OPC these are relationships with `TargetMode="External"`, and the target is an
opaque URI.

**ExyokiOffice never follows one of those targets on its own.** There is no
HTTP client in the code base, and the library never reads a file outside the
package it was handed. Opening, saving, and validating a document touch nothing
but the package itself, no matter what the document references or how the
library is configured.

When an application does have a legitimate reason to follow such a target, it
supplies the access itself, as a `std::shared_ptr` to an implementation of
`ExyokiOffice::Security::IExternalResourceResolver`. The library then asks
that resolver, after checking the target against a policy it enforces itself.
This is the same division of labour as with digital signatures and
`ICryptoProvider` (see [Signatures.md](Signatures.md)): the library owns the
document format, the application owns the world outside it.

Public headers:

- `include/ExyokiOffice/Security/ResourceResolver.hpp` — the resolver
  interface, the request and response types, and the policy.
- `include/ExyokiOffice/Security/ExternalResources.hpp` — enumerating,
  checking, and reading the external references of a package.
- `include/ExyokiOffice/Tools/ExternalResourceInspector.hpp` — a one-call
  report used by `exyoki external`.

## Everything is off by default

Two independent things have to be set before anything outside the package is
reachable, and neither one has a default that reaches anything:

| | Default | Effect of leaving it |
|---|---|---|
| Resolver | `nullptr` | Every read reports `NoResolver`. |
| Policy | `ExternalResourcePolicy::Deny()` — no schemes, no kinds | Every request reports `AccessDenied` **without calling the resolver**. |

Forgetting either one keeps the package sealed. A package that is cleared or
reloaded resets its usage budget but keeps the resolver and the policy, because
those are settings of the object rather than of the content.

## Listing what a document points at

This needs no resolver and no policy, and is worth running before deciding on
either:

```cpp
#include "ExyokiOffice/Security/ExternalResources.hpp"

for (const auto& reference : ExyokiOffice::Security::CollectExternalReferences(*document))
{
    std::cout << reference.SourcePartUri << ' ' << reference.RelationshipId << ' '
              << ExyokiOffice::Security::ToString(reference.Kind) << ' ' << reference.Target << '\n';
}
```

The kind is derived from the relationship type, so it says what the document
meant the target to be:

| Kind | Relationship type ends in |
|---|---|
| `LinkedImage` | `image` |
| `LinkedMedia` | `audio`, `video`, `media` |
| `AttachedTemplate` | `attachedTemplate` |
| `ExternalWorkbook` | `externalLink`, `externalLinkPath` |
| `LinkedOleObject` | `oleObject`, `package` |
| `Hyperlink` | `hyperlink` |
| `Unknown` | anything else |

## The policy

```cpp
ExyokiOffice::Security::ExternalResourcePolicy policy;
policy.AllowedSchemes = {"https"};
policy.AllowedHosts = {".assets.example.com"};   // the domain and its subdomains
policy.AllowedKinds = {ExyokiOffice::Security::ExternalResourceKind::LinkedImage};
policy.MaxResourceBytes = 4 * 1024 * 1024;
policy.MaxTotalBytes = 16 * 1024 * 1024;
policy.MaxRequests = 20;

document->SetExternalResourcePolicy(policy);
document->SetExternalResourceResolver(std::make_shared<MyResolver>());
```

`ExternalResourcePolicy::HttpsOnly(hosts, kinds)` is a shorthand for the common
case; every other field keeps its default, so the budgets still apply.

| Field | Meaning |
|---|---|
| `AllowedSchemes` | Lower case, no colon. Empty allows nothing. |
| `AllowedHosts` | Exact host, or `.example.com` for a domain and its subdomains. |
| `AllowedPorts` | Empty means only the scheme's own port, so an allowed host cannot be used to reach an unrelated service. |
| `AllowedPathPrefixes` | Compared against the decoded, normalized path at a directory boundary, so `/data` does not match `/database`. Empty allows any path. |
| `AllowedKinds` | Empty allows nothing. |
| `BaseUri` | Base for relative targets. Empty denies every relative target. |
| `Timeout` | Deadline passed to the resolver. |
| `MaxResourceBytes` | Largest single resource. |
| `MaxTotalBytes` | Total bytes for one package. |
| `MaxRequests` | Number of requests for one package. |
| `AllowUserInfoInUri` | Whether `https://user:secret@host/` is acceptable. Off by default. |
| `FollowRedirectOutsideAllowlist` | Whether a resolver may report content read from a URI the policy does not allow. Off by default. |

The library applies this **before** calling the resolver and checks the answer
against it **afterwards**, so a resolver that naively fetches whatever URI it is
handed still cannot be steered outside the allowlist. Concretely, the following
are refused without the resolver ever being asked:

- a scheme, host, port, path, or kind outside the allowlist;
- a URI carrying credentials;
- a relative target with no `BaseUri`;
- a `file:`/`smb:` path that navigates upwards, including through percent
  encoding such as `%2e%2e`;
- a request once `MaxRequests` or `MaxTotalBytes` is used up.

And after the resolver answers:

- more data than the request allowed becomes `TooLarge` with the payload
  dropped;
- an `EffectiveUri` outside the allowlist becomes `AccessDenied` with the
  payload dropped;
- a signalled cancellation token becomes `Cancelled` with the payload dropped;
- an exception escaping the resolver becomes `Failed`.

Windows paths are folded into the file scheme before any of this: `C:\assets\x`
becomes `file:///C:/assets/x` and `\\server\share\x` becomes
`file://server/share/x`, so a UNC share is subject to the same host allowlist
as any network target.

Every refusal is also recorded in `OpenXmlPackage::LastValidationResult()` as a
warning in `ValidationDomain::Security`, with the source part, relationship id,
relationship type, and target filled in.

## Reading a resource

```cpp
auto response = ExyokiOffice::Security::ResolveExternalResource(*document, reference);
if (response)
{
    use(response.Data);
}
else
{
    std::cerr << ExyokiOffice::Security::ToString(response.Status) << ": " << response.Message << '\n';
}
```

`CheckExternalReference` answers the policy question alone, without reading
anything and without consuming any budget. That is what fits hyperlinks and
linked OLE objects, where the question is whether the document points somewhere
it should not.

The bytes are handed back to the caller. Nothing is written into the package
unless you ask for it explicitly (see below), and opening a document never
resolves anything.

## Writing a resolver

The interface has a single method. It is called only for targets the policy has
already approved, and it is given the deadline and the byte budget so it can
stop early rather than buffer a huge response.

```cpp
class HttpsResolver final : public ExyokiOffice::Security::IExternalResourceResolver
{
public:
    ExyokiOffice::Security::ExternalResourceResponse Resolve(
        const ExyokiOffice::Security::ExternalResourceRequest& request) override
    {
        using ExyokiOffice::Security::ExternalResourceStatus;
        ExyokiOffice::Security::ExternalResourceResponse response;

        if (request.CancellationToken && request.CancellationToken->IsCancelled())
        {
            response.Status = ExternalResourceStatus::Cancelled;
            return response;
        }

        // Fetch request.Uri with your HTTP client. Honour request.Timeout, stop
        // once request.MaxBytes have been read and report TooLarge, and set
        // response.EffectiveUri when you followed a redirect.
        response.Status = ExternalResourceStatus::Ok;
        response.Data = std::move(bytes);
        response.ContentType = contentType;
        return response;
    }
};
```

A local file resolver is the same shape with `std::ifstream` behind it; the
`file:` scheme reaches the resolver just like any other, so the library still
does not touch the file system itself.

Practical notes:

- `Resolve` must not throw. The library guards against it, but a thrown
  exception is reported as an opaque `Failed`.
- Implementations must be safe to call from several threads.
- `response.ContentType` is a hint from an untrusted source. The library treats
  it as one: an embedded picture takes its format from the bytes through
  `ExyokiOffice::DetectImageFormat`, not from this field.
- `request.OriginalTarget` is the relationship target as stored, which is useful
  for logging; `request.Uri` is the absolute, normalized form the policy
  approved.

### Installing it at open time

```cpp
ExyokiOffice::Packaging::OpenSettings settings;
settings.ExternalResources = std::make_shared<HttpsResolver>();
settings.ExternalResourcePolicy = ExyokiOffice::Security::ExternalResourcePolicy::HttpsOnly(
    {".assets.example.com"}, {ExyokiOffice::Security::ExternalResourceKind::LinkedImage});

auto document = ExyokiOffice::Word::WordDocumentEditor::Open("report.docx", settings);
```

This only installs them. `Open` itself resolves nothing, no matter how
permissive the policy is.

## Per-format helpers

Each of these is a thin wrapper over `ResolveExternalResource`, and none of them
runs unless it is called.

| Format | API |
|---|---|
| Word | `WordDocument::GetAttachedTemplateReference()`, `WordDocument::ResolveAttachedTemplate()` |
| Excel | `ExcelDocumentEditor::ExternalWorkbookLinks()`, `ExcelDocumentEditor::ResolveExternalWorkbook()` |
| PowerPoint | `PresentationShape::EmbedLinkedPicture()`, `PresentationShape::EmbedLinkedMedia()` |

`EmbedLinkedPicture` is the one helper that changes the document: on success the
bytes become an image part, the `a:blip r:link` becomes `r:embed`, and the
external relationship is dropped, so the slide no longer depends on anything
outside the package. It leaves the picture untouched when the resource cannot be
read.

Attached template targets are usually relative or plain file paths, so
`ExternalResourcePolicy::BaseUri` normally has to be set for
`ResolveAttachedTemplate` to resolve at all.

## Command line

```
exyoki external report.docx
exyoki external report.docx --format json
```

Lists every outward reference with its source part, relationship id, kind, and
target. The command has no resolver and accesses nothing — it reports what the
document claims, which is exactly the audit worth running before allowing
anything.

## Security notes

- **SSRF.** A document is untrusted input. An allowlist of hosts is the control
  that matters; `MaxRequests` and `MaxTotalBytes` bound the damage of a document
  that references a thousand targets.
- **Internal networks.** The library cannot tell that `https://internal-admin/`
  is inside your perimeter. Name the hosts you mean rather than allowing a
  scheme broadly.
- **UNC and SMB.** A `\\server\share` target authenticates as the calling user
  on Windows, which is a credential leak to whoever controls the server. It goes
  through the same host allowlist as any other target, and denying the `file`
  and `smb` schemes rules it out entirely.
- **Redirects.** Leave `FollowRedirectOutsideAllowlist` off and report
  `EffectiveUri`; the library re-checks it.
- **Decompression.** The size limits apply to what the resolver returns. A
  resolver that transparently decompresses a response must count the
  decompressed bytes against `request.MaxBytes` itself.
- **Content type.** Never decide what a resource is from the type the source
  claims.

## What is not implemented

- Any network or file system access inside the library. There is no built-in
  resolver, and there will not be one.
- Caching of resolved resources. A resolver that wants a cache implements one.
- Rewriting a document's external targets in bulk, or resolving Excel external
  workbook values into cached results.
