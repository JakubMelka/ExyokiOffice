# xmlpart crash artifacts

Each file here is a fuzzer input that once made a target fail. They are replayed
by `FuzzCorpusReplayTests` in the ordinary unit test build, so a fixed defect
stays fixed. Never delete one because it "passes now" - that is the point.

## crash-e6d6f9b118725e0d8d562a67f8c3fc1714f9aa7f

Found on 2026-07-27, first campaign, within 25 seconds.

Payload (after the leading empty-XPath length byte):

```
<w:dold:w:r><w:t>&#0;o<\x03//0<root a</w/:
```

Tripped the "XML serialization is not idempotent" invariant.

`OpenXmlPackagePart::SetXmlString` parsed with
`Document.load_string(xml.c_str())`. `c_str()` ends at the first NUL, and
`&#0;` is enough to put one into the text node, so re-reading a serialized part
silently truncated the document at that point instead of parsing or rejecting
it. Fixed by parsing with `load_buffer(xml.data(), xml.size())`, which also
brings the method in line with how the package loader reads parts.

## crash-76e63d62365d506fab315151ead1b9db670b0aaf

Found immediately after the fix above, and **not a library defect**.

Payload:

```
<?xml ve\x95CTYP&l1.0#?>YPDOCT'\xb2\xd9\x93\x8b;roo\xfat><r>&#0;
```

Once `load_buffer` stopped truncating, the NUL that `&#0;` resolves to reached
the writer. A raw NUL is not a legal XML character and no writer can emit it in
a form that reads back identically, so the idempotence invariant kept firing on
a quirk of the vendored pugixml parser rather than on anything this library
does. The target now skips the second round-trip when the serialized output
contains a NUL. The input is kept because it pins that decision down: if
`SetXmlString` ever starts rejecting NUL-bearing content outright, this file is
the case to re-examine.
