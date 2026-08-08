# Threading and concurrency

ExyokiOffice operations are synchronous. The library does not create worker
threads to load, edit, validate, or save a document, and it does not provide an
asynchronous document API. Applications are free to run independent document
operations on their own threads, subject to the ownership rules in this
chapter.

The central rule is:

> A package and its complete object graph are not thread-safe. Serialize every
> access to one object graph, or confine it to one thread. Independent packages
> may be used concurrently.

## What belongs to one object graph

Treat all objects reached from one package or editor as one synchronization
domain. This includes:

- a `WordDocumentEditor`, `ExcelDocumentEditor`, or
  `PowerPointDocumentEditor`;
- the `WordDocument`, `ExcelDocument`, or `PowerPointDocument` beneath it;
- package parts, relationship collections, and content-type data;
- typed DOM elements;
- worksheets, slides, paragraphs, runs, shape trees, cursors, managers, and
  other wrappers returned by an editor;
- borrowed views, spans, iterators, and references into any of the above.

Locking only the editor while another thread uses a worksheet, slide, part, or
DOM element from the same document is not sufficient: those objects reach the
same mutable package graph.

The public contract does not guarantee that simultaneous read-only operations
on one graph are safe. Some internal data is lazily initialized or cached, and
an apparently const operation may traverse objects whose lifetime is controlled
by the same graph. If one graph must be shared, externally serialize all access,
including reads.

## Supported concurrency model

| Situation | Supported? | Requirement |
| --- | --- | --- |
| Two threads use two independently opened documents | Yes | The documents must not share wrappers or mutable application state. |
| Two threads create two new documents | Yes | Each thread owns a separate editor and output. |
| Several threads read or mutate one editor | Not directly | Protect the entire editor graph with one application-owned lock. |
| One thread saves while another reads or edits the same document | No | Serialize save against every access to that graph. |
| One thread requests cancellation of an operation in another | Yes | Use a thread-safe `ICancellationToken` implementation. |
| Several operations share a resource resolver or crypto provider | Yes | The application-supplied implementation must be thread-safe. |

Independent packages can share immutable schema metadata safely. This is an
implementation service used by the typed DOM; callers do not need to lock it.

Independent package instances are not enough when they target the same external
resource. For example, two editors saving to the same file path still require
application-level coordination because they race through the file system even
though their in-memory graphs are separate.

## Recommended pattern: one document per worker

The simplest scalable design is to give each task exclusive ownership of one
document for its entire operation:

```cpp
#include "ExyokiOffice/Word/WordDocument.hpp"

#include <filesystem>

bool ProcessDocument(const std::filesystem::path& input,
                     const std::filesystem::path& output)
{
    ExyokiOffice::Packaging::OpenSettings settings;
    settings.PackageLimits = ExyokiOffice::OpenXmlPackageLimits::Recommended();
    auto editor = ExyokiOffice::Word::WordDocumentEditor::Open(input, settings);
    if (!editor)
    {
        return false;
    }

    if (!editor->AddParagraph("Processed by ExyokiOffice"))
    {
        return false;
    }

    return editor->SaveToFile(output);
}
```

A thread pool may run this function for many input files at once, provided each
task has a distinct editor and output path. No ExyokiOffice mutex is needed
between those independent tasks.

When several tasks need the same starting document, open it separately in each
task or first obtain package bytes and open a separate editor from those bytes.
Do not hand wrappers from one editor to another worker. Creating the bytes must
itself happen while the source document is exclusively owned or locked.

## Sharing one document with a lock

When application design requires several threads to reach one document, keep
the editor and one mutex in the same owner object. Every method that touches the
editor or anything obtained from it must take that mutex:

```cpp
#include "ExyokiOffice/Word/WordDocument.hpp"

#include <filesystem>
#include <mutex>
#include <string>
#include <utility>

class SharedWordDocument
{
public:
    explicit SharedWordDocument(
        ExyokiOffice::Word::WordDocumentEditor::Ptr editor)
        : m_editor(std::move(editor))
    {
    }

    bool AppendParagraph(const std::string& text)
    {
        const std::scoped_lock lock(m_mutex);
        return m_editor && m_editor->AddParagraph(text) != nullptr;
    }

    bool Save(const std::filesystem::path& path)
    {
        const std::scoped_lock lock(m_mutex);
        return m_editor && m_editor->SaveToFile(path);
    }

private:
    std::mutex m_mutex;
    ExyokiOffice::Word::WordDocumentEditor::Ptr m_editor;
};
```

Do not return an editor wrapper or DOM pointer from such a locked method and use
it after the lock is released. Perform the complete operation inside the
critical section, or return an application-owned value that no longer refers to
the document graph.

A reader-writer lock is appropriate only if the application can prove that its
read path is entirely read-only and safe for every object it touches. The
library does not make that guarantee generally, so an exclusive mutex is the
default recommendation.

## Cooperative cancellation across threads

Long-running load and save operations accept an optional
`ICancellationToken`. The token is polled at safe checkpoints; cancellation does
not terminate a thread and is not instantaneous. An open operation returns no
partially initialized document, and an atomic save does not publish a partial
target file.

The token is non-owning. It must remain alive until the operation returns. If a
different thread changes its state, both the write and `IsCancelled()` must be
thread-safe:

```cpp
#include "ExyokiOffice/ICancellationToken.hpp"

#include <atomic>

class CancellationFlag final : public ExyokiOffice::ICancellationToken
{
public:
    void Cancel() noexcept
    {
        m_cancelled.store(true, std::memory_order_relaxed);
    }

    bool IsCancelled() const override
    {
        return m_cancelled.load(std::memory_order_relaxed);
    }

private:
    std::atomic_bool m_cancelled = false;
};
```

Calling `Cancel()` is the cross-thread operation. The thread performing the
load or save must still own, or exclusively lock, the package it is operating
on. Cancellation does not make concurrent document access safe.

## Application-supplied callbacks

Two extension interfaces have their own concurrency requirement:

- `Security::IExternalResourceResolver` must be safe to call concurrently.
- `Security::ICryptoProvider` must be safe to call concurrently for signature
  verification.

This matters when the same shared provider is installed on independent packages
processed by different application threads. Protect mutable caches, network
clients, key handles, and diagnostic state inside the implementation, or use
thread-safe dependencies. The library keeps the supplied `shared_ptr` alive for
an operation, but ownership does not serialize calls.

Resolver and provider methods must also follow their documented error contract;
in particular, a resolver must not use exceptions as its normal failure path.
See [External resources](ExternalResources.md) and
[Digital signatures](Signatures.md) for the complete policies.

## Transactions are not a synchronization mechanism

`BeginTransaction()` permits at most one active edit transaction per editor.
The transaction ownership state uses atomics so a nested or competing attempt
can be rejected and so transaction lifetime can be detached safely when an
editor is destroyed.

This guard does **not** make document mutation, snapshot creation, rollback, or
commit thread-safe. A transaction is an all-or-nothing editing facility, not a
lock. Start, use, commit, or roll back a transaction while the editor graph is
exclusively owned or protected by the same application mutex as every other
access.

Rollback replaces the editor's complete low-level graph. Wrappers, cursors,
parts, and DOM nodes obtained before rollback refer to the abandoned graph and
must not be reused, from any thread.

## Build parallelism is unrelated

The `-Jobs` options in `WinBuild.ps1`, `WinLint.ps1`, and `WinFuzz.ps1` control
compiler jobs, analysis processes, or independent fuzz targets. They do not
change the runtime thread-safety of ExyokiOffice documents.

## Checklist

Before using ExyokiOffice from several threads, verify that:

- each concurrently processed document has a distinct package graph;
- no wrapper, cursor, part, DOM node, span, or iterator crosses between owners;
- every access to a shared graph uses one common external mutex;
- no two tasks save to the same output path without coordination;
- cancellation tokens use atomic or otherwise synchronized state and outlive
  the operation;
- shared resource resolvers and crypto providers are thread-safe;
- transactions are protected by the document's normal ownership or lock.
