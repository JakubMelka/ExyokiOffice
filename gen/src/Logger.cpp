// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "Logger.h"
#include <nlohmann/json.hpp>

#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace exyoki::generator
{
namespace
{
std::mutex g_logMutex;
std::vector<WarningDiagnostic> g_warnings;

std::string Timestamp()
{
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto time = system_clock::to_time_t(now);

    std::tm tm;
#if defined(_WIN32)
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

void Write(std::string_view level, std::string_view message, std::ostream& output)
{
    output << '[' << Timestamp() << "] [" << level << "] " << message << '\n';
}

void WriteCompilerStyleWarning(const WarningDiagnostic& diagnostic)
{
    const std::string code = diagnostic.code.empty() ? "GEN-WARNING" : diagnostic.code;
    if (!diagnostic.sourceFile.empty())
    {
        std::cerr << diagnostic.sourceFile << "(1): warning " << code << ": "
                  << diagnostic.message << '\n';
        return;
    }

    std::cerr << "OpenXmlGenerator: warning " << code << ": "
              << diagnostic.message << '\n';
}

void SetIfNotEmpty(nlohmann::json& object, std::string_view name, const std::string& value)
{
    if (!value.empty())
    {
        object[std::string(name)] = value;
    }
}
} // namespace

void Logger::Info(std::string_view message)
{
    std::lock_guard lock(g_logMutex);
    Write("info", message, std::cout);
}

void Logger::Warn(WarningDiagnostic diagnostic)
{
    std::lock_guard lock(g_logMutex);
    WriteCompilerStyleWarning(diagnostic);
    g_warnings.push_back(std::move(diagnostic));
}

void Logger::Error(std::string_view message)
{
    std::lock_guard lock(g_logMutex);
    Write("error", message, std::cerr);
}

void Logger::ResetWarnings()
{
    std::lock_guard lock(g_logMutex);
    g_warnings.clear();
}

std::size_t Logger::WarningCount()
{
    std::lock_guard lock(g_logMutex);
    return g_warnings.size();
}

void Logger::WriteWarningReport(const std::filesystem::path& path)
{
    std::lock_guard lock(g_logMutex);

    if (path.has_parent_path())
    {
        std::filesystem::create_directories(path.parent_path());
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        throw std::runtime_error("Unable to write generator warning report: " + path.string());
    }

    std::map<std::string, std::size_t, std::less<>> categoryCounts;
    std::map<std::string, std::size_t, std::less<>> codeCounts;
    std::map<std::string, std::size_t, std::less<>> ignoredPropertyCounts;
    for (const auto& warning : g_warnings)
    {
        ++categoryCounts[warning.category];
        ++codeCounts[warning.code];
        if (warning.code == "GEN-IGNORED-PROPERTY")
        {
            ++ignoredPropertyCounts[warning.property];
        }
    }

    nlohmann::json report = {
        {"formatVersion", 1},
        {"warningCount", g_warnings.size()},
        {"categoryCounts", categoryCounts},
        {"codeCounts", codeCounts},
        {"ignoredPropertyCounts", ignoredPropertyCounts},
        {"warnings", nlohmann::json::array()}};
    for (const auto& warning : g_warnings)
    {
        nlohmann::json entry = nlohmann::json::object();
        SetIfNotEmpty(entry, "code", warning.code);
        SetIfNotEmpty(entry, "category", warning.category);
        SetIfNotEmpty(entry, "message", warning.message);
        SetIfNotEmpty(entry, "sourceFile", warning.sourceFile);
        SetIfNotEmpty(entry, "ownerKind", warning.ownerKind);
        SetIfNotEmpty(entry, "ownerName", warning.ownerName);
        SetIfNotEmpty(entry, "property", warning.property);
        report["warnings"].push_back(std::move(entry));
    }
    output << std::setw(2) << report << '\n';

    if (!output)
    {
        throw std::runtime_error("Failed while writing generator warning report: " + path.string());
    }
}
} // namespace exyoki::generator
