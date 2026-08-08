// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "FuzzTargets.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// This is the regression half of SEC-006. The fuzz entry points are ordinary
// functions, so every committed corpus seed and every committed crash artifact
// can be replayed here, in a plain MSVC build, without libFuzzer or Clang.
//
// Reproducing a fuzzer finding therefore does not require a fuzz build: drop
// the artifact into tests/fuzz/crashes/<target>/ and it runs as part of
// ctest from then on.
namespace
{

std::vector<ExyokiOffice::Byte> ReadFile(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return std::vector<ExyokiOffice::Byte>(std::istreambuf_iterator<char>(stream),
                                           std::istreambuf_iterator<char>());
}

/// Replays every regular file in @p directory through @p entry.
ExyokiOffice::Size ReplayDirectory(const std::filesystem::path& directory,
                                   ExyokiOffice::Fuzz::FuzzEntryPoint entry)
{
    if (!std::filesystem::is_directory(directory))
    {
        return 0;
    }

    ExyokiOffice::Size replayed = 0;
    for (const auto& file : std::filesystem::directory_iterator(directory))
    {
        if (!file.is_regular_file())
        {
            continue;
        }

        // README files document what a crash artifact was; they are notes, not
        // inputs, and replaying them would only add noise.
        if (file.path().filename() == "README.md")
        {
            continue;
        }

        const auto bytes = ReadFile(file.path());

        INFO("replaying ", file.path().string());
        CHECK(entry(bytes.data(), bytes.size()) == 0);
        ++replayed;
    }

    return replayed;
}

} // namespace

TEST_CASE("Fuzz corpus replays cleanly [unit] [fuzz]")
{
    const std::filesystem::path fuzzDir(EXYOKIOFFICE_FUZZ_DIR);
    REQUIRE(std::filesystem::is_directory(fuzzDir));

    ExyokiOffice::Size totalReplayed = 0;
    for (const auto& target : ExyokiOffice::Fuzz::AllTargets())
    {
        const std::string name(target.Name);

        CAPTURE(name);
        totalReplayed += ReplayDirectory(fuzzDir / "corpus" / name, target.Entry);
        totalReplayed += ReplayDirectory(fuzzDir / "crashes" / name, target.Entry);
    }

    // Guards against the corpus silently disappearing - an empty replay would
    // otherwise pass and quietly remove all regression coverage.
    CHECK(totalReplayed > 0);
}

TEST_CASE("Fuzz entry points tolerate degenerate input [unit] [fuzz]")
{
    // The fuzzer generates these constantly, so they must be safe by
    // construction rather than by corpus coverage.
    static constexpr ExyokiOffice::UInt8 oneByte[] = {0};

    for (const auto& target : ExyokiOffice::Fuzz::AllTargets())
    {
        const std::string name(target.Name);
        CAPTURE(name);

        CHECK(target.Entry(nullptr, 0) == 0);
        CHECK(target.Entry(oneByte, 0) == 0);
        CHECK(target.Entry(oneByte, 1) == 0);
    }
}
