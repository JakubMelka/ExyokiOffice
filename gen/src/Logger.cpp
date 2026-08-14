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
/// File-local state and formatting behind the generator log.
class LoggerHelper
{
public:
    inline static std::mutex g_logMutex;
    inline static std::vector<WarningDiagnostic> g_warnings;

    static std::string Timestamp()
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

    static void Write(std::string_view level, std::string_view message, std::ostream& output)
    {
        output << '[' << Timestamp() << "] [" << level << "] " << message << '\n';
    }

    static void WriteCompilerStyleWarning(const WarningDiagnostic& diagnostic)
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

    static void SetIfNotEmpty(nlohmann::json& object, std::string_view name, const std::string& value)
    {
        if (!value.empty())
        {
            object[std::string(name)] = value;
        }
    }
};

void Logger::Info(std::string_view message)
{
    std::lock_guard lock(LoggerHelper::g_logMutex);
    LoggerHelper::Write("info", message, std::cout);
}

void Logger::Warn(WarningDiagnostic diagnostic)
{
    std::lock_guard lock(LoggerHelper::g_logMutex);
    LoggerHelper::WriteCompilerStyleWarning(diagnostic);
    LoggerHelper::g_warnings.push_back(std::move(diagnostic));
}

void Logger::Error(std::string_view message)
{
    std::lock_guard lock(LoggerHelper::g_logMutex);
    LoggerHelper::Write("error", message, std::cerr);
}

void Logger::ResetWarnings()
{
    std::lock_guard lock(LoggerHelper::g_logMutex);
    LoggerHelper::g_warnings.clear();
}

std::size_t Logger::WarningCount()
{
    std::lock_guard lock(LoggerHelper::g_logMutex);
    return LoggerHelper::g_warnings.size();
}

void Logger::WriteWarningReport(const std::filesystem::path& path)
{
    std::lock_guard lock(LoggerHelper::g_logMutex);

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
    for (const auto& warning : LoggerHelper::g_warnings)
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
        {"warningCount", LoggerHelper::g_warnings.size()},
        {"categoryCounts", categoryCounts},
        {"codeCounts", codeCounts},
        {"ignoredPropertyCounts", ignoredPropertyCounts},
        {"warnings", nlohmann::json::array()}};
    for (const auto& warning : LoggerHelper::g_warnings)
    {
        nlohmann::json entry = nlohmann::json::object();
        LoggerHelper::SetIfNotEmpty(entry, "code", warning.code);
        LoggerHelper::SetIfNotEmpty(entry, "category", warning.category);
        LoggerHelper::SetIfNotEmpty(entry, "message", warning.message);
        LoggerHelper::SetIfNotEmpty(entry, "sourceFile", warning.sourceFile);
        LoggerHelper::SetIfNotEmpty(entry, "ownerKind", warning.ownerKind);
        LoggerHelper::SetIfNotEmpty(entry, "ownerName", warning.ownerName);
        LoggerHelper::SetIfNotEmpty(entry, "property", warning.property);
        report["warnings"].push_back(std::move(entry));
    }
    output << std::setw(2) << report << '\n';

    if (!output)
    {
        throw std::runtime_error("Failed while writing generator warning report: " + path.string());
    }
}
} // namespace exyoki::generator
