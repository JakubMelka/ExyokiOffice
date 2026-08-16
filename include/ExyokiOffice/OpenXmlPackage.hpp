// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/ICancellationToken.hpp"
#include "ExyokiOffice/OpenXmlPackagePart.hpp"
#include "ExyokiOffice/Security/ResourceResolver.hpp"
#include "ExyokiOffice/ValidationResult.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ExyokiOffice
{

struct OpenXmlPackageImpl;
class OpenXmlPackageValidator;

namespace Security
{
class ExternalResourceGateway;
}

/**
 * @brief ZIP and XML ceilings the OPC loader enforces while reading a package.
 *
 * Every field is an independent limit and **zero means unlimited**. A
 * default-constructed value therefore enforces nothing, which is why it is not
 * what a package starts with: packages start at Recommended(), so a caller who
 * never thinks about limits still gets a defence against decompression bombs
 * ("zip bombs") and deeply nested XML. `{}` and Unlimited() switch that off,
 * and are the right answer only for packages from a source you already trust.
 *
 * @warning An application that knows its own documents should tighten these
 * rather than accept Recommended() as final. It is sized so that no ordinary
 * Office document is rejected, which makes it much wider than what a specific
 * application needs — a server accepting uploads in particular wants a far
 * smaller MaxUncompressedBytes. Pick values from what the application's own
 * documents actually need rather than from the format's theoretical maxima.
 *
 * Limits are checked while loading and reject the package rather than
 * truncating it: exceeding any of them makes the load fail, and the reason is
 * reported through OpenXmlPackage::LastValidationResult() as
 * ValidationErrorId::OpcLimitExceeded or ValidationErrorId::XmlLimitExceeded.
 *
 * Use Recommended() to get values sized for ordinary Office documents instead
 * of writing them out by hand, and Unlimited() to say "no limits" explicitly.
 *
 * @code
 * Packaging::OpenSettings settings;
 * settings.PackageLimits = OpenXmlPackageLimits::Recommended();
 * auto editor = Word::WordDocumentEditor::Open("untrusted.docx", settings);
 * @endcode
 *
 * @see OpenXmlPackage::SetPackageLimits
 * @see Packaging::OpenSettings::PackageLimits
 */
struct EXYOKIOFFICE_EXPORT OpenXmlPackageLimits
{
    /** @brief Maximum number of ZIP entries in the package. */
    UInt64 MaxEntries = 0;
    /** @brief Maximum total compressed size of all entries, in bytes. */
    UInt64 MaxCompressedBytes = 0;
    /** @brief Maximum total uncompressed size of all entries, in bytes. */
    UInt64 MaxUncompressedBytes = 0;
    /** @brief Maximum uncompressed size of any single part, in bytes. */
    UInt64 MaxPartBytes = 0;
    /** @brief Maximum number of relationships in any one relationships part. */
    UInt64 MaxRelationships = 0;
    /**
     * @brief Maximum tolerated uncompressed-to-compressed size ratio of an entry.
     *
     * This is the primary decompression-bomb guard: a small entry that inflates
     * far beyond this ratio is rejected before its payload is fully read.
     */
    UInt64 MaxCompressionRatio = 0;
    /** @brief Maximum XML element nesting depth in any part. */
    UInt64 MaxXmlDepth = 0;
    /** @brief Maximum total number of XML nodes in any one part. */
    UInt64 MaxXmlNodes = 0;
    /** @brief Maximum total number of XML attributes in any one part. */
    UInt64 MaxXmlAttributes = 0;
    /** @brief Maximum total length of all XML text and CDATA in any one part, in characters. */
    UInt64 MaxXmlTextCharacters = 0;

    /**
     * @brief Limits sized for ordinary Office documents; the default for new packages.
     *
     * A sensible starting point for opening documents you do not control. The
     * values are far above what ordinary `.docx`, `.xlsx`, and `.pptx` files
     * need, so they should not reject legitimate content, while still bounding
     * memory and cutting off decompression bombs. They are a policy choice, not
     * a format requirement, and may be re-tuned in a future minor release.
     *
     * An application that knows its own documents should tighten these; a
     * server accepting uploads in particular wants a much smaller
     * MaxUncompressedBytes than the general-purpose value used here.
     */
    [[nodiscard]] static OpenXmlPackageLimits Recommended() noexcept;

    /**
     * @brief No limits at all — identical to a default-constructed value.
     *
     * Prefer this over `{}` when the intent is deliberate, so that reviewers can
     * tell "no limits were wanted here" apart from "limits were forgotten".
     * Only appropriate for packages from a source you already trust.
     */
    [[nodiscard]] static OpenXmlPackageLimits Unlimited() noexcept;

    /**
     * @brief Why one ZIP entry is refused by these limits; empty when it fits.
     *
     * The loader is not the only reader of a package: `Tools::Unpack` extracts
     * the same archives without going through it, and a guard only one of them
     * applies is a guard an attacker picks their way around. Both call these,
     * so there is one definition of what "too big" means and one set of
     * messages to keep truthful.
     *
     * Checked from the ZIP directory, before the entry is decompressed — that
     * is what makes it a defence rather than a report on the damage.
     */
    [[nodiscard]] std::string_view CheckEntry(UInt64 uncompressedBytes, UInt64 compressedBytes) const noexcept;

    /// Why the entry count is refused by these limits; empty when it fits.
    [[nodiscard]] std::string_view CheckEntryCount(UInt64 entryCount) const noexcept;

    /// Why the accumulated package totals are refused; empty when they fit.
    [[nodiscard]] std::string_view CheckTotals(UInt64 compressedTotal, UInt64 uncompressedTotal) const noexcept;
};

/**
 * @brief Controls whether the loader keeps the bytes it read for every part.
 *
 * A part is normally parsed and the source bytes are dropped, because saving
 * re-serializes the tree. A digital signature, however, digests the byte stream
 * as it was stored, so verifying a signature written by another application
 * requires the original bytes. Retention roughly doubles the memory used by XML
 * parts, which is why it is not unconditional.
 */
enum class PartByteRetention
{
    /** @brief Never keep the source bytes; signatures can then not be verified. */
    Never,
    /** @brief Keep them only for packages that actually contain signature parts (default). */
    WhenSignaturesPresent,
    /** @brief Always keep them. */
    Always
};

/**
 * @brief What saving should do when the package already carries digital signatures.
 *
 * Editing a signed package invalidates its signatures, and so does saving one
 * that was merely loaded, because XML parts are re-serialized rather than copied
 * byte for byte. The policy decides how visible that is. The check itself is
 * exact: the digests stored in the signatures are recomputed against the bytes
 * the save is about to write.
 */
enum class SignatureSavePolicy
{
    /** @brief Save silently; the signatures stay in the package and stop verifying. */
    Ignore,
    /** @brief Save, but record a warning in LastValidationResult() (default). */
    Warn,
    /** @brief Remove all signature parts before writing, leaving an unsigned package. */
    RemoveSignatures,
    /** @brief Refuse to save; the save operation fails and nothing is written. */
    FailSave
};

/**
 * @brief High level abstraction over Open XML packages (.docx, .pptx, .xlsx, custom).
 *
 * OpenXmlPackage is both a container (it inherits OpenXmlPartContainer) and the gateway
 * to the physical package. It understands the Open Packaging Conventions:
 *  - Parts are stored as entries inside a ZIP archive.
 *  - Relationship edges are persisted in `_rels/.rels` files and express how parts
 *    relate to each other (for example, `/word/document.xml` -> `/word/styles.xml`).
 *  - Content types are declared globally in `[Content_Types].xml` and can be overridden
 *    per part.
 *
 * The class hides the low-level ZIP and XML mechanics so that derived types can focus on
 * domain-specific logic. Even if you have never touched Open XML before, the following
 * checklist helps to orient yourself:
 *  1. Load or create a package via LoadFromFile/LoadFromMemory.
 *  2. Use generated helper methods (e.g. `GetWorkbookPart()`) to access parts.
 *  3. Traverse relationships via Parts()/Relationships() or Lookup by URI.
 *  4. Save the modified package back to disk or memory.
 *
 * @par Error reporting
 * The library does not throw. Load and save operations report failure through
 * their return value and record the detail in LastValidationResult().
 *
 * @par Thread safety
 * A package and the parts and DOM elements it owns are **not** thread-safe.
 * Loading, saving, and any mutation of one package must be externally
 * serialized against every other access to that package, including reads.
 * Distinct packages may be used concurrently from distinct threads; the schema
 * metadata behind the typed DOM is immutable and shared safely.
 *
 * @code
 * auto package = std::make_shared<Packaging::WordprocessingDocument>();
 * package->LoadFromFile("report.docx");
 * auto mainPart = package->GetMainDocumentPart();
 * auto doc = mainPart->GetDocument(); // strongly typed root element
 * // ... manipulate DOM ...
 * package->SaveToFile("updated.docx");
 * @endcode
 */
class EXYOKIOFFICE_EXPORT OpenXmlPackage : public OpenXmlPartContainer,
                                           public std::enable_shared_from_this<OpenXmlPackage>
{
public:
    using Ptr = std::shared_ptr<OpenXmlPackage>;

    explicit OpenXmlPackage();
    virtual ~OpenXmlPackage();

    /**
     * @brief Loads the package from disk.
     *
     * The previous package state is cleared before loading. If loading fails or
     * cancellation is requested, the object is left empty rather than partially
     * initialized.
     *
     * @param path Source package path.
     * @param cancellationToken Optional non-owning token used to cancel loading.
     * @return true when the package was loaded successfully; false on I/O,
     *         parsing, validation, or cancellation failure.
     */
    bool LoadFromFile(const std::filesystem::path& path,
                      const ICancellationToken* cancellationToken = nullptr);

    /**
     * @brief Saves the package to disk.
     *
     * When atomicSave is true, the package is written to a sibling temporary file
     * first and then published to the destination path. When a cancellation token
     * is supplied, the method also uses a temporary file even if atomicSave is
     * false, so cancellation cannot leave a partially written destination.
     *
     * @param path Destination package path.
     * @param atomicSave When true, publish through a temporary file and rename.
     * @param cancellationToken Optional non-owning token used to cancel saving.
     * @return true when the package was saved and published; false on write,
     *         publish, BeforeSave, or cancellation failure.
     */
    bool SaveToFile(const std::filesystem::path& path,
                    bool atomicSave = true,
                    const ICancellationToken* cancellationToken = nullptr);

    /**
     * @brief Loads a complete package from memory.
     *
     * The previous package state is cleared before loading. If loading fails or
     * cancellation is requested, the object is left empty rather than partially
     * initialized.
     *
     * @param buffer Complete OPC ZIP package bytes.
     * @param cancellationToken Optional non-owning token used to cancel loading.
     * @return true when the package was loaded successfully; false on parsing,
     *         validation, or cancellation failure.
     */
    bool LoadFromMemory(std::span<const Byte> buffer,
                        const ICancellationToken* cancellationToken = nullptr);

    /**
     * @brief Serializes the entire package to an in-memory buffer.
     *
     * If cancellation is requested, no partial buffer is returned.
     *
     * @param cancellationToken Optional non-owning token used to cancel saving.
     * @return Serialized package bytes, or an empty vector on save or
     *         cancellation failure.
     */
    std::vector<Byte> SaveToMemory(const ICancellationToken* cancellationToken = nullptr);

    /**
     * @brief Returns the Office application family this package belongs to.
     *
     * Generated document classes report their own family. It decides where the
     * few parts that move with the application are stored, such as the web
     * extension task panes part. A plain OpenXmlPackage reports Unknown.
     */
    [[nodiscard]] virtual OpenXmlDocumentFamily DocumentFamily() const noexcept
    {
        return OpenXmlDocumentFamily::Unknown;
    }

    /// Returns the part that matches the provided normalized URI.
    std::shared_ptr<OpenXmlPackagePart> GetPartByUri(std::string_view uri) const;

    /// Returns diagnostics captured by the most recent load/open policy.
    const ValidationResult& LastValidationResult() const noexcept;
    /**
     * @brief Configures ZIP/XML safety limits enforced by subsequent load operations.
     *
     * Affects later loads only; a package already in memory is not re-checked.
     * A package starts at OpenXmlPackageLimits::Recommended(), or at the
     * process-wide policy when one was installed; pass Unlimited() to lift the
     * limits deliberately.
     */
    void SetPackageLimits(OpenXmlPackageLimits limits);
    [[nodiscard]] const OpenXmlPackageLimits& PackageLimits() const noexcept;

    /**
     * @brief Sets the limits every package constructed from now on starts with.
     *
     * For an application that opens documents it does not control and reaches
     * the loader through code it does not own — the `ExyokiOffice::Tools`
     * entry points construct their own packages, and so do the high-level
     * editors — there is otherwise no seam to pass limits through. Setting the
     * default once during start-up covers all of them at once.
     *
     * Unconfigured, packages start at OpenXmlPackageLimits::Recommended(), so
     * an application that never calls this is still not defenceless against a
     * hostile package. Use this to install tighter values than the general
     * purpose defaults, or an explicit Unlimited() for a process that only ever
     * sees its own files. `exyoki` and the MCP servers call it at start-up, so
     * their `--package-limits` reaches every layer.
     *
     * Passing std::nullopt returns the process to the unconfigured state.
     *
     * This is a start-up switch, not a per-operation control: call it once,
     * before any package is loaded and before other threads run. Reading it is
     * synchronized, so a late call cannot tear a value, but it still leaves
     * packages constructed earlier — and Packaging::OpenSettings values
     * constructed earlier, which capture this default too — on the old limits,
     * which is exactly the confusion the "set it once" rule avoids. Where a
     * seam does exist, prefer
     * SetPackageLimits or Packaging::OpenSettings::PackageLimits: an explicit
     * limit at the open site outlives anyone forgetting the global one.
     */
    static void SetDefaultPackageLimits(std::optional<OpenXmlPackageLimits> limits);
    [[nodiscard]] static OpenXmlPackageLimits DefaultPackageLimits();

    /**
     * @brief The configured default, or std::nullopt when none was ever set.
     *
     * DefaultPackageLimits() answers "what do packages start with", which
     * cannot distinguish a deliberate Unlimited() from Recommended() having
     * been substituted for a choice nobody made. A layer that must tell the two
     * apart asks this instead, so that an explicit `--package-limits unlimited`
     * is never silently overruled by a safer default.
     */
    [[nodiscard]] static std::optional<OpenXmlPackageLimits> ConfiguredDefaultPackageLimits();

    /// Configures original byte retention applied by subsequent load operations.
    void SetPartByteRetention(PartByteRetention retention) noexcept;
    [[nodiscard]] PartByteRetention GetPartByteRetention() const noexcept;

    /// Configures what saving does when the package carries digital signatures.
    void SetSignatureSavePolicy(SignatureSavePolicy policy) noexcept;
    [[nodiscard]] SignatureSavePolicy GetSignatureSavePolicy() const noexcept;

    /**
     * @brief Makes the next save leave the save-time document properties alone.
     *
     * Saving normally refreshes `dcterms:modified` in `docProps/core.xml`. That
     * would rewrite a part a digital signature has just digested, so the
     * signature would no longer verify against the file that was written.
     * Security::SignPackage arms this flag for exactly that reason, and callers
     * rarely need to set it themselves.
     *
     * The suppression is **one-shot**: the next save consumes it and writes the
     * package exactly as it was signed. Any later save updates the properties
     * again, on the assumption that the document has been edited since — which
     * does invalidate the signature, and the configured SignatureSavePolicy
     * then decides how loudly that is reported.
     *
     * @see Security::SignPackage
     * @see SetSignatureSavePolicy
     */
    void SuppressSaveTimePropertyUpdateOnce() noexcept;

    /** @return True while the next save will skip the save-time property update. */
    [[nodiscard]] bool IsSaveTimePropertyUpdateSuppressed() const noexcept;

    /**
     * @brief Installs the interface used to read resources outside the package.
     *
     * The library never fetches anything itself. A document can point outward
     * through external relationships (a linked image, an attached template, a
     * workbook another workbook links to), and following such a target is only
     * possible once the application supplies a resolver. Without one, every
     * request reports Security::ExternalResourceStatus::NoResolver.
     *
     * Installing a resolver is not by itself enough: the target must also pass
     * the external resource policy, which denies everything by default.
     *
     * Nothing in loading, saving, or validating calls the resolver.
     */
    void SetExternalResourceResolver(Security::IExternalResourceResolver::Ptr resolver);
    [[nodiscard]] const Security::IExternalResourceResolver::Ptr& GetExternalResourceResolver() const noexcept;

    /**
     * @brief Configures which external targets may be reached and under which limits.
     *
     * The default policy allows nothing. See Security::ExternalResourcePolicy for
     * the allowlist and budget fields; the library enforces them before calling
     * the resolver and checks the response against them afterwards.
     */
    void SetExternalResourcePolicy(Security::ExternalResourcePolicy policy);
    [[nodiscard]] const Security::ExternalResourcePolicy& GetExternalResourcePolicy() const noexcept;

    /**
     * @brief Clears the consumed request count and byte total for external resources.
     *
     * The budget is per package and is also reset by Clear() and by loading.
     */
    void ResetExternalResourceBudget() noexcept;

    /// Removes every part and resets content type dictionaries.
    void Clear();

protected:
    virtual std::filesystem::path ContainerPath() const override;
    virtual std::string ContainerUri() const override;
    virtual bool BeforeSave() { return true; }

    /**
     * @brief Reads and clears the one-shot save-time property suppression.
     *
     * Derived packages call this at the top of BeforeSave(). A true result
     * means this save must not touch the document properties, because the
     * package was signed and the signature covers them.
     *
     * @return True when this save has to leave the properties untouched.
     */
    [[nodiscard]] bool ConsumeSaveTimePropertyUpdateSuppression() noexcept;

    void ClearValidationResult();
    void SetLastValidationResult(ValidationResult result);

    /// Adds a package-level relationship (source "/") and assigns a URI to the part.
    void AttachRootPart(const std::shared_ptr<OpenXmlPackagePart>& part,
                        const OpenXmlPartDescriptor& descriptor,
                        bool allowMultiple);
    /// Removes a package-level relationship and detaches the part from the package.
    bool DetachRootPart(const std::shared_ptr<OpenXmlPackagePart>& part);
    /// Inserts a part into the global URI map (called automatically by containers).
    void RegisterPart(const std::shared_ptr<OpenXmlPackagePart>& part);
    /// Removes a part from the global URI map.
    void UnregisterPart(const std::string& uri);
    /// Returns true when a part with the specified URI already exists.
    [[nodiscard]] bool IsPartUriInUse(const std::string& uri) const;

private:
    friend class OpenXmlPartContainer;
    friend class OpenXmlPackageValidator;
    friend class Security::ExternalResourceGateway;
    friend struct OpenXmlPackageImpl;

    void ClearPackageState(bool preserveValidationResult);

    /// Applies the signature save policy; returns false when the save must stop.
    bool ApplySignatureSavePolicy();

    std::unique_ptr<OpenXmlPackageImpl> m_impl;
};

} // namespace ExyokiOffice
