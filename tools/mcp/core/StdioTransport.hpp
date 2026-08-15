// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/StandardTypes.hpp"

#include <iosfwd>
#include <string>
#include <string_view>

namespace ExyokiOffice::Mcp
{

/**
 * @brief Newline-delimited JSON transport over a pair of streams.
 *
 * MCP stdio framing is one UTF-8 JSON value per line; there are no
 * `Content-Length` headers (that framing belongs to the Language Server
 * Protocol, not to MCP). The default construction reads standard input and
 * writes standard output, which is why nothing else in the servers may ever
 * write to stdout — every diagnostic goes to stderr.
 *
 * A different input stream is used by the `--replay` mode, which feeds a
 * recorded `.jsonl` conversation through the same code path the real client
 * would drive.
 */
class StdioTransport
{
public:
    /// Binds the transport to standard input and standard output.
    StdioTransport();

    /// Binds the transport to explicit streams (replay mode and tests).
    StdioTransport(std::istream& input, std::ostream& output);

    /**
     * @brief Largest message accepted, in bytes.
     *
     * A line is read into memory before anything can tell whether it is a
     * message at all, so the length has to be bounded before the parser sees
     * it: without a cap a single unterminated line is an out-of-memory
     * condition rather than a protocol error. Sixteen mebibytes is far above
     * any real request — inline media is capped by `--max-media-size`, which
     * defaults to eight — and far below what hurts.
     */
    static constexpr Size MaximumLineBytes = 16u * 1024u * 1024u;

    /**
     * @brief Reads the next line from the input stream.
     *
     * A line longer than MaximumLineBytes is not returned. It is consumed to
     * its end so the next read starts on a message boundary rather than in the
     * middle of the oversized one, @p line is cleared, and @p oversized is set
     * so the caller can answer with a parse error instead of silently ignoring
     * a request the client believes it sent.
     *
     * @return False at end of input, which the server treats as a clean shutdown.
     */
    bool ReadLine(std::string& line, bool& oversized);

    /// Reads the next line, ignoring whether an oversized one was dropped.
    bool ReadLine(std::string& line);

    /// Writes one message followed by a newline and flushes.
    void WriteLine(std::string_view line);

private:
    std::istream* m_input = nullptr;
    std::ostream* m_output = nullptr;
};

} // namespace ExyokiOffice::Mcp
