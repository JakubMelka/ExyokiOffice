// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Packaging/PackageUtilities.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <fstream>

namespace ExyokiOffice::Packaging
{

namespace
{
std::vector<Byte> ReadStreamInternal(std::istream& stream)
{
    std::vector<Byte> buffer;
    if (!stream)
    {
        return buffer;
    }

    stream.seekg(0, std::ios::end);
    const auto length = stream.tellg();
    if (length < 0)
    {
        stream.clear();
        stream.seekg(0, std::ios::beg);
        return buffer;
    }

    buffer.resize(static_cast<Size>(length));
    stream.seekg(0, std::ios::beg);
    stream.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(length));
    if (!stream)
    {
        buffer.clear();
        stream.clear();
    }
    return buffer;
}
} // namespace

std::vector<Byte> ReadFileFully(const std::filesystem::path& path)
{
    if (path.empty())
    {
        return {};
    }

    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return {};
    }

    return ReadStreamInternal(file);
}

std::vector<Byte> ReadStreamFully(std::iostream& stream)
{
    stream.clear();
    return ReadStreamInternal(stream);
}

bool WriteStream(std::iostream& stream, const std::vector<Byte>& data)
{
    stream.clear();
    stream.seekp(0, std::ios::beg);
    if (!stream && !data.empty())
    {
        stream.clear();
        return false;
    }

    if (!data.empty())
    {
        stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    stream.flush();
    stream.seekg(0, std::ios::beg);
    return static_cast<bool>(stream);
}

std::string BuildRelationshipTarget(const std::filesystem::path& path)
{
    auto generic = path.generic_string();
    if (generic.empty())
    {
        return path.string();
    }
    return generic;
}

} // namespace ExyokiOffice::Packaging
