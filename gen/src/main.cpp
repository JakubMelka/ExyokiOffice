// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "Generator.h"
#include "Logger.h"

#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

using exyoki::generator::Generator;
using exyoki::generator::GeneratorConfig;

/// File-local helpers for the generator entry point.
class GeneratorMainHelper
{
public:
    struct Arguments
    {
        std::filesystem::path dataRoot;
        std::filesystem::path includeOut;
        std::filesystem::path sourceOut;
        std::filesystem::path testsOut;
        std::filesystem::path warningReport;
        bool generateNamespaces = true;
        bool generateParts = true;
        bool generateSchema = true;
        bool generateTests = true;
    };

    static void PrintUsage()
    {
        std::cout << "Usage: OpenXmlGenerator --data <path> --out-include <path> --out-source <path> [options]\n"
                     "Options:\n"
                     "  --out-tests <path>       Emit generated per-namespace DOM unit tests here\n"
                     "  --warning-report <path>  Write the structured compatibility warning report here\n"
                     "  --no-namespaces    Disable namespace generation\n"
                     "  --no-parts         Disable part generation\n"
                     "  --no-schema        Disable schema generation\n"
                     "  --no-tests         Disable generated DOM unit-test generation\n";
    }

    static Arguments ParseArguments(int argc, char** argv)
    {
        Arguments args;
        for (int i = 1; i < argc; ++i)
        {
            std::string_view current(argv[i]);
            auto RequireValue = [&](std::string_view option) -> std::filesystem::path
            {
                if (i + 1 >= argc)
                {
                    throw std::runtime_error("Missing value for option: " + std::string(option));
                }
                return std::filesystem::path(argv[++i]);
            };

            if (current == "--data")
            {
                args.dataRoot = RequireValue(current);
            }
            else if (current == "--out-include")
            {
                args.includeOut = RequireValue(current);
            }
            else if (current == "--out-source")
            {
                args.sourceOut = RequireValue(current);
            }
            else if (current == "--out-tests")
            {
                args.testsOut = RequireValue(current);
            }
            else if (current == "--warning-report")
            {
                args.warningReport = RequireValue(current);
            }
            else if (current == "--no-namespaces")
            {
                args.generateNamespaces = false;
            }
            else if (current == "--no-parts")
            {
                args.generateParts = false;
            }
            else if (current == "--no-schema")
            {
                args.generateSchema = false;
            }
            else if (current == "--no-tests")
            {
                args.generateTests = false;
            }
            else
            {
                throw std::runtime_error("Unknown option: " + std::string(current));
            }
        }

        if (args.dataRoot.empty() || args.includeOut.empty() || args.sourceOut.empty())
        {
            throw std::runtime_error("Required options --data, --out-include, and --out-source must be provided.");
        }

        return args;
    }
};

int main(int argc, char** argv)
{
    std::filesystem::path warningReport;
    try
    {
        if (argc == 1)
        {
            GeneratorMainHelper::PrintUsage();
            return 0;
        }

        auto args = GeneratorMainHelper::ParseArguments(argc, argv);
        warningReport = args.warningReport.empty()
                            ? std::filesystem::current_path() / "OpenXmlGeneratorWarnings.json"
                            : std::filesystem::absolute(args.warningReport);
        exyoki::generator::Logger::ResetWarnings();

        GeneratorConfig config;
        config.dataRoot = std::filesystem::absolute(args.dataRoot);
        config.outputInclude = std::filesystem::absolute(args.includeOut);
        config.outputSource = std::filesystem::absolute(args.sourceOut);
        config.outputTests = args.testsOut.empty()
                                 ? std::filesystem::path{}
                                 : std::filesystem::absolute(args.testsOut);
        config.generateNamespaces = args.generateNamespaces;
        config.generateParts = args.generateParts;
        config.generateSchema = args.generateSchema;
        config.generateTests = args.generateTests;

        Generator generator(std::move(config));
        generator.Run();
        exyoki::generator::Logger::WriteWarningReport(warningReport);
        exyoki::generator::Logger::Info(
            "Wrote " + std::to_string(exyoki::generator::Logger::WarningCount()) + " warnings to '" + warningReport.string() + "'.");
    }
    catch (const std::exception& ex)
    {
        if (!warningReport.empty())
        {
            try
            {
                exyoki::generator::Logger::WriteWarningReport(warningReport);
            }
            catch (const std::exception& reportError)
            {
                exyoki::generator::Logger::Error(reportError.what());
            }
        }
        exyoki::generator::Logger::Error(ex.what());
        return 1;
    }

    return 0;
}
