// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "StdioTransport.hpp"

#include <iostream>
#include <string>

namespace ExyokiOffice::Mcp
{

StdioTransport::StdioTransport()
    : m_input(&std::cin), m_output(&std::cout)
{
}

StdioTransport::StdioTransport(std::istream& input, std::ostream& output)
    : m_input(&input), m_output(&output)
{
}

bool StdioTransport::ReadLine(std::string& line, bool& oversized)
{
    oversized = false;
    line.clear();

    // Read character by character rather than with std::getline, which grows
    // its string until the line ends or memory runs out: the cap has to apply
    // while the line is being read, not after.
    for (;;)
    {
        const auto next = m_input->get();
        if (next == std::char_traits<char>::eof())
        {
            // A final line without a trailing newline is still a message; only
            // an empty tail means there is nothing left to hand back.
            return !line.empty() && !oversized;
        }

        const auto character = static_cast<char>(next);
        if (character == '\n')
        {
            break;
        }

        if (oversized)
        {
            // Still consuming the rest of the oversized line: nothing is kept,
            // but the newline has to be reached so the next read starts on a
            // message boundary.
            continue;
        }

        if (line.size() >= MaximumLineBytes)
        {
            oversized = true;
            line.clear();
            continue;
        }

        line.push_back(character);
    }

    // Windows clients often write CRLF; the trailing carriage return would
    // otherwise end up inside the JSON payload handed to the parser.
    if (!line.empty() && line.back() == '\r')
    {
        line.pop_back();
    }

    return true;
}

bool StdioTransport::ReadLine(std::string& line)
{
    bool oversized = false;
    return ReadLine(line, oversized);
}

void StdioTransport::WriteLine(std::string_view line)
{
    m_output->write(line.data(), static_cast<std::streamsize>(line.size()));
    m_output->put('\n');
    m_output->flush();
}

} // namespace ExyokiOffice::Mcp
