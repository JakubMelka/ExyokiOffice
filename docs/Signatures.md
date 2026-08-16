# Digital signatures

ExyokiOffice can verify the digital signatures a package already carries and
create new ones. It links **no cryptographic library**: everything that needs a
private key or an X.509 certificate is delegated to an implementation of
`ExyokiOffice::Security::ICryptoProvider` that you write and pass in as a
`std::shared_ptr`. The library owns the OPC and XML side of the problem —
canonicalization, manifests, transforms and digests — and it computes SHA-1/2
digests itself.

Public headers:

- `include/ExyokiOffice/Security/CryptoProvider.hpp` — the provider interface
  and the algorithm identifiers.
- `include/ExyokiOffice/Security/PackageSignatures.hpp` — verification,
  signing, and removal.
- `include/ExyokiOffice/Tools/SignatureInspector.hpp` — a one-call report used
  by `exyoki signatures`.

## What a package signature is

A signed OPC package contains a signature origin part
(`/_xmlsignatures/origin.sigs`, empty) related from the package root, and one
XML signature part per signature (`/_xmlsignatures/sig1.xml`). Each signature
follows XML Signature with the OPC profile of ECMA-376 Part 2:

- `Object` `idPackageObject` holds a `Manifest` with one `Reference` per signed
  part. The reference URI is the part URI plus a `?ContentType=` query, and the
  digest is computed over the part's **stored byte stream**.
- Relationship parts are covered through the
  `.../package/2006/RelationshipTransform` transform, which selects
  relationships by `SourceId` or `SourceType`, sorts them by identifier, makes
  `TargetMode` explicit, and canonicalizes the result. Because of that, adding
  an unrelated relationship later does not break the signature.
- `Object` `idOfficeObject` holds the optional Office details
  (`SignatureInfoV1`), and the package object also carries `mdssi:SignatureTime`.
- `SignedInfo` references those `Object` elements by fragment, and the signature
  value is computed over the canonical form of `SignedInfo`.

Canonicalization is inclusive XML canonicalization 1.0
(`REC-xml-c14n-20010315`). Exclusive canonicalization is recognized but not
implemented; a signature that uses it is reported as unsupported rather than
silently mis-verified. Canonicalization also has its own bounded nesting check:
signature XML that is too deeply nested fails closed as
`SignatureError::CanonicalizationFailed` rather than recursing until the
process stack is exhausted. This guard is independent of package-open limits
because signatures may also be created from an already constructed DOM.

## Verification

```cpp
#include "ExyokiOffice/Security/PackageSignatures.hpp"

auto document = ExyokiOffice::Word::WordDocument::Open("signed.docx");
auto result = ExyokiOffice::Security::VerifySignatures(*document, provider);

for (const auto& signature : result.Signatures)
{
    std::cout << signature.PartUri
              << " content: " << static_cast<int>(signature.ContentIntegrity)
              << " value: "   << static_cast<int>(signature.SignatureValue) << '\n';
}
```

Verification has two independent halves:

| Check | Question it answers | Needs a provider |
|---|---|---|
| `ContentIntegrity` | Has any signed part changed since it was signed? | no |
| `SignatureValue` | Was this really signed with that certificate's key? | yes |

`IsValid()` requires both. Without a provider `SignatureValue` stays
`NotChecked`, so a signature is never reported as valid on the strength of
unchanged content alone — "unchanged" is not "signed by someone".

`ContentIntegrity` answers that question only about the parts the signature
demonstrably covers, and which parts those are is decided the strict way. The
list of covered parts is read from the `Manifest` inside a `dsig:Object` whose
own digest has just verified, never from any `Manifest` that happens to sit
somewhere in the signature part. The difference matters: the same-document
digest and the signature value survive rearranging the `Object` elements
around the signed one, so a verifier that looked for the manifest by position
could be shown a signature whose manifest had been moved out of view — it
would then check no part at all and call the package intact. Two related
refusals follow from the same rule:

- A signature none of whose verified references names a package part or a
  relationship set covers no content. It is reported as `Invalid` with
  `SignatureMalformed`, never as valid content, because it is evidence about
  itself and about nothing else. The test is on what the references resolve
  to, not on whether a `Manifest` was there: a manifest whose entries are all
  bare-name `#id` references digests elements of the signature XML, so it is
  neither empty nor covering.
- A signature part that gives two elements the same `Id` is refused outright,
  and the root `Signature` element counts as one of them. Which element a
  `#fragment` reference resolves to would otherwise be up to the
  implementation, and this library and Word could disagree about what was
  checked.

The certificates are handed over as raw DER (`SignatureResult::Certificates`);
the library never parses them. Chain building, revocation checking and trust
decisions belong to your provider and to the policy of your application.

### What the signature does not cover

A valid signature says the parts its manifest names have not changed. It says
nothing about the parts it does not name, and nothing in the file announces
which those are — a part added after signing simply is not mentioned, every
digest still matches, and `IsValid()` is still true. The relationship transform
makes that reachable in practice: a signature that selects relationships by
`SourceId` keeps its digest when a relationship with a new identifier is added
beside them, and the part that relationship points at comes along with it.

`SignatureResult::UncoveredParts` is the list, sorted by URI, with the signature
parts and their origin excluded because they cannot sign themselves:

```cpp
for (const auto& uri : signature.UncoveredParts)
{
    std::cerr << "not covered by the signature: " << uri << '
';
}
```

An entry is not by itself a defect — Office signs a subset on purpose. What it
means is your application's decision: a new slide in a signed deck is a
different matter from an unreferenced thumbnail.

### SHA-1

A signature commits to a digest, so a digest collisions can be constructed for
is a digest two different documents share, however strong the key is. SHA-1
collisions have been constructible since 2017 and chosen-prefix collisions since
2020, so a SHA-1 reference or an RSA-SHA1 signature value is reported as
`Invalid` rather than computed.

Office still writes SHA-1 signatures in old compatibility modes and archives are
full of them, so this is a policy rather than a format this library cannot read:

```cpp
ExyokiOffice::Security::VerifySignaturesOptions options;
options.AllowSha1 = true;  // read the resulting Valid as "unmodified, assuming
                           // nobody constructed a collision"
```

### Original bytes

A signature digests the bytes a part had in the file, and saving re-serializes
XML parts rather than copying them. Verification therefore needs the loaded
bytes, which the OPC loader keeps according to
`ExyokiOffice::PartByteRetention`:

- `WhenSignaturesPresent` (default) — kept for packages that carry signatures.
- `Always` — always kept, at the cost of roughly doubling the memory used for
  XML parts.
- `Never` — never kept; references over XML parts are then reported as
  `NotChecked`.

The policy is set with `OpenXmlPackage::SetPartByteRetention` or through
`OpenSettings::ByteRetention`.

## Saving a signed package invalidates its signatures

This is worth stating plainly: **ExyokiOffice does not round-trip XML parts
byte for byte.** Parts are parsed into a DOM and written back out by the XML
writer, which is free to differ in whitespace and formatting from what the
original application wrote. Saving a signed package therefore breaks its
signatures even when you changed nothing.

`OpenXmlPackage::SetSignatureSavePolicy` decides what happens:

| Policy | Behavior |
|---|---|
| `Warn` (default) | Saves, and records a `SignatureInvalidatedBySave` warning in `LastValidationResult()`. |
| `FailSave` | Refuses to save; `SaveToFile` returns false and nothing is written. |
| `RemoveSignatures` | Drops the signature parts, leaving a valid unsigned package. |
| `Ignore` | Saves silently, leaving signatures that will no longer verify. |

The check is exact rather than a guess: the digests stored in the signature are
recomputed against the bytes the save is about to write, so a save that happens
to produce identical bytes passes without a warning.

`ExyokiOffice::Security::CheckSignaturesBeforeSave` runs the same check on
demand, so an application can ask before it offers to save.

## Signing

```cpp
ExyokiOffice::Security::SignPackageOptions options;
options.Digest = ExyokiOffice::Security::DigestAlgorithm::Sha256;
options.Office.Comments = "Approved by the accounting department";

const auto result = ExyokiOffice::Security::SignPackage(*document, provider, options);
if (!result)
{
    std::cerr << result.Message << '\n';
    return 1;
}
document->SaveToFile("signed.docx");
```

The signature covers the parts as they would be written at that moment, so sign
immediately before saving and do not modify the document in between. By default
every part except the signature parts is covered, together with the relationship
parts; `SignPackageOptions::PartUris` narrows that down.

Signing asks the provider for exactly two things: the certificate chain, and a
signature over the canonicalized `SignedInfo`. Everything else is built by the
library.

## Writing a crypto provider

The interface is small on purpose. A provider must report the algorithm it
signs with, hand over its certificate chain in DER, and implement signing and
verification of raw bytes. `ComputeDigest` already has a default implementation
backed by the library's own SHA code, so override it only to route hashing
through your own backend as well.

```cpp
class MyProvider final : public ExyokiOffice::Security::ICryptoProvider
{
public:
    ExyokiOffice::Security::SignatureAlgorithm GetSignatureAlgorithm() const override
    {
        return ExyokiOffice::Security::SignatureAlgorithm::RsaSha256;
    }

    std::vector<std::vector<Byte>> GetCertificateChain() const override
    {
        return {m_certificateDer};
    }

    bool SignData(ExyokiOffice::Security::SignatureAlgorithm algorithm,
                  std::span<const Byte> data,
                  std::vector<Byte>& signature) const override
    {
        // Hash data with the digest belonging to algorithm and sign it with the
        // private key; write the raw signature value into signature.
    }

    bool VerifyData(ExyokiOffice::Security::SignatureAlgorithm algorithm,
                    std::span<const Byte> data,
                    std::span<const Byte> signature,
                    std::span<const Byte> certificateDer) const override
    {
        // Return true only when signature verifies against the public key in
        // certificateDer.
    }

private:
    std::vector<Byte> m_certificateDer;
};
```

Practical backends:

- **Windows**: CNG (`BCryptSignHash`, `BCryptVerifySignature`) or CryptoAPI
  (`CryptSignHash`), with the certificate coming from `CertOpenStore` /
  `CertFindCertificateInStore`. Both are operating system APIs, so no extra
  dependency is needed.
- **Unix**: OpenSSL (`EVP_DigestSign`, `EVP_DigestVerify`, `d2i_X509`).
- **Hardware**: PKCS#11 or a remote signing service; the interface never asks
  for the private key itself, only for the result of signing.

Implementations must be safe to call from several threads for verification.

## Checking an interoperable signature by hand

The unit tests use a deterministic stub provider that performs no cryptography,
which proves the plumbing but cannot prove interoperability. To confirm that a
signature produced here is accepted by other tools:

1. Create a self-signed test certificate
   (`New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=ExyokiOffice Test"`)
   and write a provider over CNG that uses it.
2. Sign a `.docx` with `SignPackage` and save it.
3. Open the document in Word: *File ▸ Info ▸ View Signatures* has to list the
   signature and report the document as unmodified since signing. A self-signed
   certificate will additionally be reported as untrusted, which is expected.
4. Editing anything and saving must make Word report the signature as invalid.

## Command line

```
exyoki signatures signed.docx
exyoki signatures signed.docx --format json
```

The command lists every signature with its algorithms, signing time, number of
certificates, and the state of each reference. It checks content integrity only:
the command line has no crypto provider, so it never reports on the signature
value. Exit code `8` means the package carries a signature whose content no
longer matches.

## What is not implemented

- OOXML package encryption (agile or standard). Signing and encryption are
  separate problems; encryption remains out of scope for now.
- Certificate parsing, chain building, revocation checking, and time stamping.
- Exclusive XML canonicalization, XPath transforms, and enveloped signature
  transforms.
- Countersignatures and signature lines with visual representations.
