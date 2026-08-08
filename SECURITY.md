# Security policy

ExyokiOffice reads Office Open XML packages that its callers frequently do not
control — a `.docx` arriving by mail, an `.xlsx` uploaded to a service.
Parsing untrusted input is therefore part of the library's job, and a
memory-safety defect in that path is a security bug rather than an ordinary one.

This page explains how to report such a defect, what the library defends
against on its own, and what it deliberately leaves to the application.

## Supported versions

Security fixes are made on the latest released version; older releases are not
patched retroactively. The released number lives in `VERSION.txt`.

A fix is still bound by the versioning policy in [docs/ABI.md](docs/ABI.md):
only a patch release promises an unchanged ABI, so a fix that alters a public
type's layout, a virtual table, or the exported symbol set ships as at least a
minor release even when existing code still compiles against it.

## Reporting a vulnerability

**Report privately through GitHub, not in a public issue or pull request.**
Open a draft advisory with *Report a vulnerability* on the repository's
[Security tab](https://github.com/JakubMelka/EverydayOffice/security/advisories/new).
The report stays visible only to you and the maintainers until an advisory is
published.

Please include, as far as you can establish it:

- the operating system, compiler, and CMake preset;
- the library version or commit;
- which entry point receives the untrusted data — a document load, a specific
  `exyoki` command, a `Tools` or DOM call;
- a minimal reproduction, and the expected versus actual result;
- the sanitizer report, if the defect was found under AddressSanitizer or
  UndefinedBehaviorSanitizer;
- the package limits in force (see below), because a resource-exhaustion
  report means something different with and without them.

Attach the smallest input that still reproduces the problem. Reduce and
sanitize any real document first: **do not send files containing confidential
information.** A fuzzer-derived input is welcome as the raw artifact, in the
form the corpus already uses under `tests/fuzz/crashes/<target>/` — see
[docs/fuzzing.md](docs/fuzzing.md).

## What happens next

A report is triaged first, and the severity decides the pace:

- **Critical** — memory corruption or another exploitable defect reachable by
  loading an ordinary document, or a signature verified as valid over content
  it does not cover. **A fix is developed within one week of the report being
  confirmed**, and released as soon as it is tested.
- **Everything else** — fixed on the normal development schedule and shipped
  with the next release.

The sequence is the same in both cases: the report is acknowledged, the fix is
developed privately together with a regression test, a release is cut, and a
GitHub Security Advisory is published afterwards, with a CVE where one is
warranted. Reporters are credited in the advisory and in `CHANGELOG.md` unless
they ask otherwise.

If the one week cannot be met — because the reproduction is not yet
understood, or the fix turns out to need an ABI break — you will be told
where it stands rather than left waiting. This is a small project without a
staffed security rotation, so please do not read silence as dismissal.

Please keep the details private until the advisory is published, and tell us
if you have a disclosure deadline of your own so it can be planned for rather
than discovered.

## What the library defends against

Four subsystems carry the security-relevant behavior. Each has a user manual
chapter; what matters here is which side of the boundary a defect falls on.

### ZIP and XML limits — off unless you set them

`OpenXmlPackageLimits` bounds entry counts, compressed and uncompressed sizes,
compression ratio, XML depth, node and attribute counts, and text length.
**Every limit defaults to zero, which means unlimited.** They are the only
mechanism bounding a decompression bomb or deeply nested XML, so a
default-constructed configuration has no defence against either:

```cpp
Packaging::OpenSettings settings;
settings.PackageLimits = OpenXmlPackageLimits::Recommended();
auto editor = Word::WordDocumentEditor::Open("untrusted.docx", settings);
```

Exceeding a limit rejects the package — the load fails and reports
`ValidationErrorId::OpcLimitExceeded` or `ValidationErrorId::XmlLimitExceeded`
— rather than truncating it. A service accepting uploads should tighten the
recommended values to what its own documents actually need.

Not every open has a seam to pass settings through: the `ExyokiOffice::Tools`
entry points construct their own packages, and so do the high-level editors when
called without `OpenSettings`. `OpenXmlPackage::SetDefaultPackageLimits` sets
what every package constructed afterwards starts with, which is how an
application covers those paths at once:

```cpp
OpenXmlPackage::SetDefaultPackageLimits(OpenXmlPackageLimits::Recommended());
```

Call it once during start-up, before any package is loaded. It is a fallback,
not an override: an explicit `SetPackageLimits` or
`OpenSettings::PackageLimits` still wins.

**`ExyokiOffice::Tools` does not wait to be told.** `Stat`, `Diff`, `Detect`,
`Redact`, `Unpack`, `Query`, `Extract` and their neighbours are pointed at files
the caller did not produce, and most of them take no settings at all, so the
module defaults to `Recommended()` on its own — see `Tools::DefaultPackageLimits`.
An application that installed a process-wide policy gets that policy instead,
including a deliberate `Unlimited()`. Everything reachable through `exyoki` and
the MCP servers therefore runs bounded whether or not the front end remembered
to say so.

**`exyoki` and the three MCP servers** default to `--package-limits recommended`
and install it process-wide; `--package-limits unlimited` restores the library
default for the cases where the limits are what stands in the way.

Memory unsafety, or a limit that fails to stop the input it names, is a
vulnerability. Memory or CPU exhaustion under
`OpenXmlPackageLimits::Unlimited()` is a configuration choice, not one.

### External resources — off by default, twice

A document can point outside itself: a linked image, an attached template, an
external workbook, a linked OLE object. **ExyokiOffice never follows one of
those targets on its own.** There is no HTTP client in the code base, and the
library never reads a file outside the package it was handed.

Reaching anything requires both an application-supplied
`IExternalResourceResolver` (default `nullptr`) and a policy that permits the
scheme and kind (default `ExternalResourcePolicy::Deny()`). Leaving either one
alone keeps the package sealed. See
[docs/ExternalResources.md](docs/ExternalResources.md).

Anything that reaches a target the policy did not permit, or that bypasses the
gateway entirely, is a vulnerability. What an application's own resolver
chooses to fetch is that application's responsibility — SSRF through a
permissive resolver is a property of the resolver, not of the library.

### Digital signatures — no cryptographic code is linked

Signature verification and creation delegate every private-key and certificate
operation to an application-supplied `ICryptoProvider`. The library owns the
OPC and XML side — canonicalization, manifests, transforms, digests. See
[docs/Signatures.md](docs/Signatures.md).

Reporting a signature as valid over content it does not actually cover — a
tampered part, a part outside the manifest, a mishandled relationship
transform — is a vulnerability. A weakness in the provider you supply, or in
an algorithm you selected, is not.

### Document protection is not encryption

Word document protection, Excel workbook and worksheet protection, and
PowerPoint modify protection store a password *verifier* — the value the
Office application checks before lifting a restriction it is applying itself.
Word and PowerPoint write the salted, iterated ISO/IEC 29500 hash; Excel
worksheets use the interoperable legacy verifier Excel expects there, which is
weak by construction. Either way the package stays plain OOXML: every part
remains readable, and any tool that ignores the setting can rewrite the
document. It guards against accidental edits, not against a determined reader.
Recovering content past such a verifier is not a vulnerability — see
[docs/word/protection.md](docs/word/protection.md).

Encrypted OOXML files are a different container format (a compound file rather
than a ZIP package) and cannot be opened at all;
[docs/Compatibility.md](docs/Compatibility.md) records this.

## Out of scope

- **Third-party code.** pugixml (`sources/pugixml`), miniz
  (`sources/zip/miniz.h`), and the single-header libraries under `3rdparty`
  are vendored. Report defects in them upstream — but if you can reach one
  through this library's public API with an ordinary document, we want to know,
  because the fix may belong in the wrapping code or in the limits.
- **Dependency freshness.** Dependabot covers only the GitHub Actions used by
  the workflows; the vendored single-header libraries are refreshed by hand, as
  `.github/dependabot.yml` explains. An outdated vendored copy is a maintenance
  issue unless a concrete exploitable path through the API comes with it.
- **The build and CI infrastructure.** Every workflow is manual
  (`workflow_dispatch`) and builds nothing on its own; see
  [docs/ci.md](docs/ci.md).
- **Generated documents.** Producing a document that some other application
  mishandles is an interoperability bug — report it as a normal issue.

## Hardening checklist for applications

When the documents come from outside your own system:

1. Set `OpenXmlPackageLimits::Recommended()` or tighter on every open, and call
   `OpenXmlPackage::SetDefaultPackageLimits` once at start-up to cover the
   `ExyokiOffice::Tools` entry points, which construct their own packages.
2. Leave the external-resource resolver unset unless you need it; if you do
   need it, grant the narrowest policy that works and enforce your own timeouts
   and size caps inside the resolver.
3. Check `LastValidationResult()` and treat a failed load as a rejected
   document rather than retrying with the limits removed.
4. Do not infer confidentiality or authenticity from document protection;
   verify a package signature for authenticity, and encrypt at the storage or
   transport layer for confidentiality.
5. Isolate bulk conversion of untrusted documents in a process you can kill,
   with its own memory limit.
