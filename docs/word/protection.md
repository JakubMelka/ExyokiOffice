# Document protection

Document protection restricts editing in Word-compatible applications. It is
*not* encryption: the package stays plain OOXML, every part remains
readable, and any tool that ignores `w:documentProtection` can still rewrite
the document. Use it as a safeguard against accidental edits, not for
confidentiality.

```cpp
#include "ExyokiOffice/Word/WordDocument.hpp"
using namespace ExyokiOffice::Word;
```

## Applying, inspecting, and removing protection

```cpp
WordProtectionOptions options;
options.Editing = WordProtectionType::TrackedChanges; // revision tracking cannot be turned off
options.RestrictFormattingToUnlockedStyles = true;

auto applied = editor->ProtectDocument(options, "review");
auto state = editor->GetDocumentProtection();          // options plus hasPassword
auto removed = editor->UnprotectDocument("review");
```

`WordProtectionType` covers Word's five editing restrictions (`None`,
`ReadOnly`, `Comments`, `TrackedChanges`, `Forms`); at least one editing or
formatting restriction must be requested. Set `options.Enforce = false` to
record restrictions that Word should not apply yet — Word's UI calls this
"stop protection" state.

`ProtectDocument` and `UnprotectDocument` return a `WordProtectionResult`
with a structured `WordProtectionError`, so failure modes (wrong password,
nothing to protect, unsupported verifier) are distinguishable in code.

## How the password works

The optional password is stored as the ISO/IEC 29500 verifier — a salted
SHA-512 hash iterated 100 000 times — which is what current Word writes. The
verifier only lets Word recognize the correct password before lifting the
restriction; it does not encrypt anything. `UnprotectDocument` validates the
password the same way before removing the element.

Documents carrying a pre-2010 legacy verifier are read correctly but report
`UnsupportedVerifier` instead of being unprotected with a guessed password.

## Related protection mechanisms

- Excel worksheets have their own protection with per-permission flags — see
  [Worksheets](../excel/worksheets.md).
- PowerPoint has modify protection ("password to modify") — see
  [Presentations](../powerpoint/presentations.md).
- Individual content controls can lock their content or their own deletion —
  see [Notes, comments, and content controls](notes.md).
