# simpletypes crash artifacts

Each file here is a fuzzer input that once made a target fail. They are replayed
by `FuzzCorpusReplayTests` in the ordinary unit test build, so a fixed defect
stays fixed. Never delete one because it "passes now" - that is the point.

Layout: the first byte selects the simple type, the rest is the text to parse.

## crash-87740757950f226bae4768153d9a96c1fe20f113

Found on 2026-07-27, first campaign, within 40 seconds.

Selector `14` (`DateTimeValue`), text `0001-01-01T00:00:00Z`.

Tripped the "parse/format round-trip is not stable" invariant.

`DateTimeValueTraits::Format` started with

```cpp
duration_cast<std::chrono::nanoseconds>(value.time_since_epoch())
```

`std::chrono::nanoseconds` counts in a signed 64-bit integer, which spans only
about the years 1678 to 2262. xsd:dateTime runs from year 1 to year 9999, so
every date outside that window overflowed and formatted as a completely
different instant - silent data corruption on save, with no diagnostic.

`0001-01-01T00:00:00Z` is not an exotic input either: it is the conventional
null-date sentinel in Office documents.

Fixed by flooring to whole seconds in the clock's own representation first and
converting only the sub-second remainder to nanoseconds.

The same input reappeared on 2026-08-06, this time from the other end. That fix
covered `Format`; `TryParse` still handed its whole-second count to
`duration_cast<system_clock::duration>`, and on a standard library whose clock
counts nanoseconds - libstdc++ and libc++, but not the Microsoft one, which uses
100 ns ticks - scaling year 1 to nanoseconds overflows. UndefinedBehaviorSanitizer
reports it as `signed integer overflow: -62135596800 * 1000000000`; without a
sanitizer it is a wrapped, arbitrary instant. Only the ASan+UBSan CI job sees it,
and only there because the same date is out of the clock's range to begin with.

Fixed by range-checking the whole-second count against what the clock's own
duration can hold and rejecting the parse when it cannot: a date the value type
has no room for is now `false` from `AssignFromString` rather than a wrong
instant. `DateTimeValue` therefore parses the year 1 sentinel where the standard
library's clock reaches that far and refuses it where it does not.
