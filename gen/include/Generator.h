// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include <filesystem>

namespace exyoki::generator
{
struct GeneratorConfig
{
    std::filesystem::path dataRoot;
    std::filesystem::path outputInclude;
    std::filesystem::path outputSource;
    std::filesystem::path outputTests;
    bool generateNamespaces = true;
    bool generateParts = true;
    bool generateSchema = true;
    bool generateTests = true;
};

class Generator
{
public:
    explicit Generator(GeneratorConfig config);
    void Run();

private:
    GeneratorConfig config_;
};
} // namespace exyoki::generator
