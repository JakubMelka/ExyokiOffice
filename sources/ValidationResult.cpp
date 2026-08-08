// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/ValidationResult.hpp"

#include <algorithm>
#include <utility>

namespace ExyokiOffice
{

namespace
{

ValidationIssue MakeIssue(ValidationSeverity severity, ValidationErrorId id, std::string message, XmlLocation location)
{
    ValidationIssue issue;
    issue.Severity = severity;
    issue.Id = id;
    issue.Message = std::move(message);
    issue.Location = location;
    return issue;
}

} // namespace

bool ValidationResult::IsValid() const noexcept
{
    return !HasErrors();
}

bool ValidationResult::HasErrors() const noexcept
{
    return std::any_of(issues_.begin(), issues_.end(), [](const ValidationIssue& issue)
                       { return issue.Severity == ValidationSeverity::Error; });
}

bool ValidationResult::HasWarnings() const noexcept
{
    return std::any_of(issues_.begin(), issues_.end(), [](const ValidationIssue& issue)
                       { return issue.Severity == ValidationSeverity::Warning; });
}

const std::vector<ValidationIssue>& ValidationResult::Issues() const noexcept
{
    return issues_;
}

void ValidationResult::AddIssue(ValidationIssue issue)
{
    issues_.push_back(std::move(issue));
}

void ValidationResult::Report(ValidationIssue issue)
{
    AddIssue(std::move(issue));
}

void ValidationResult::AddError(ValidationErrorId id, std::string message, XmlLocation location)
{
    AddIssue(MakeIssue(ValidationSeverity::Error, id, std::move(message), location));
}

void ValidationResult::AddWarning(ValidationErrorId id, std::string message, XmlLocation location)
{
    AddIssue(MakeIssue(ValidationSeverity::Warning, id, std::move(message), location));
}

void ValidationResult::Merge(const ValidationResult& other)
{
    issues_.insert(issues_.end(), other.issues_.begin(), other.issues_.end());
}

} // namespace ExyokiOffice
