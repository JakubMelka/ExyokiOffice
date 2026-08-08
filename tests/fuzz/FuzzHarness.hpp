// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

/**
 * @brief Invariant check that survives NDEBUG.
 *
 * Fuzz builds are compiled as RelWithDebInfo, where assert() expands to
 * nothing. An invariant that quietly disappears in the only configuration the
 * fuzzer ever runs in is worse than no invariant at all, so this macro aborts
 * unconditionally. std::abort() is what libFuzzer and the sanitizers recognize
 * as a crash, which is exactly the reporting path wanted here.
 */
#define EXYOKIOFFICE_FUZZ_CHECK(condition, message)                                          \
    do                                                                                       \
    {                                                                                        \
        if (!(condition))                                                                    \
        {                                                                                    \
            std::fprintf(stderr, "fuzz invariant failed: %s\n  at %s:%d\n  condition: %s\n", \
                         (message), __FILE__, __LINE__, #condition);                         \
            std::fflush(stderr);                                                             \
            std::abort();                                                                    \
        }                                                                                    \
    } while (false)

namespace ExyokiOffice::Fuzz
{

/**
 * @brief Sequential reader that turns a fuzzer input buffer into typed values.
 *
 * A fuzz target usually needs a few decisions (which parser to call, how many
 * operations to perform) plus a payload. ByteTape reads those decisions off the
 * front of the buffer and hands the rest over as the payload.
 *
 * Running out of input is not an error: every reader returns a defined value
 * once the tape is exhausted, so a target never has to check for truncation.
 * That keeps the targets short and makes short inputs - which the fuzzer
 * produces constantly - exercise the same code path as long ones.
 */
class ByteTape
{
public:
    ByteTape(const UInt8* data, Size size) noexcept
        : m_data(data), m_size(data ? size : 0)
    {
    }

    /// Returns the next byte, or 0 when the tape is exhausted.
    UInt8 NextByte() noexcept
    {
        if (m_position >= m_size)
        {
            return 0;
        }
        return m_data[m_position++];
    }

    /// Returns the next byte reduced into [0, bound); returns 0 when bound is 0.
    UInt8 NextChoice(UInt8 bound) noexcept
    {
        if (bound == 0)
        {
            return 0;
        }
        return static_cast<UInt8>(NextByte() % bound);
    }

    /// Returns true when at least one byte is still available.
    bool HasMore() const noexcept { return m_position < m_size; }

    /// Number of bytes not consumed yet.
    Size Remaining() const noexcept { return m_size - m_position; }

    /// Consumes and returns everything that is left, as text.
    std::string_view RestAsText() noexcept
    {
        const auto* begin = m_data + m_position;
        const auto length = Remaining();
        m_position = m_size;
        return std::string_view(reinterpret_cast<const char*>(begin), length);
    }

    /**
     * @brief Consumes a length-prefixed chunk and returns it as text.
     *
     * The length is a single byte, so one chunk never exceeds 255 characters.
     * When fewer bytes remain than the prefix asks for, the rest of the tape is
     * returned instead.
     */
    std::string_view NextChunk() noexcept
    {
        Size length = NextByte();
        length = length < Remaining() ? length : Remaining();

        const auto* begin = m_data + m_position;
        m_position += length;
        return std::string_view(reinterpret_cast<const char*>(begin), length);
    }

private:
    const UInt8* m_data = nullptr;
    Size m_size = 0;
    Size m_position = 0;
};

/**
 * @brief Package limits every fuzz target must install before loading.
 *
 * OpenXmlPackageLimits defaults to all zeros, which means unlimited. Left at
 * the default a coverage guided fuzzer immediately discovers zip bombs and
 * multi-gigabyte parts, reports them as out-of-memory, and then spends the rest
 * of the campaign rediscovering the same class of input instead of exploring
 * the parsers. The values below are small enough to keep every input cheap and
 * still large enough for any realistic document shape.
 *
 * Resource exhaustion is a real concern for the library, but it is a separate
 * question from memory safety and is covered by its own unit tests rather than
 * by the fuzzer.
 */
inline OpenXmlPackageLimits SafeLimits() noexcept
{
    OpenXmlPackageLimits limits;
    limits.MaxEntries = 256u;
    limits.MaxCompressedBytes = 4ull * 1024ull * 1024ull;
    limits.MaxUncompressedBytes = 16ull * 1024ull * 1024ull;
    limits.MaxPartBytes = 4ull * 1024ull * 1024ull;
    limits.MaxRelationships = 512u;
    limits.MaxCompressionRatio = 200u;
    limits.MaxXmlDepth = 100u;
    limits.MaxXmlNodes = 100000u;
    limits.MaxXmlAttributes = 100000u;
    limits.MaxXmlTextCharacters = 4ull * 1024ull * 1024ull;
    return limits;
}

/**
 * @brief Upper bound on the input size a target is willing to process.
 *
 * libFuzzer is told the same number through -max_len. The check is repeated
 * here so that a corpus file replayed by the unit tests behaves identically to
 * a fuzzer generated input.
 */
inline constexpr Size kMaxInputSize = Size{64} * 1024;

} // namespace ExyokiOffice::Fuzz
