// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "CommandCatalog.hpp"
#include "ExitCodes.hpp"

#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace exyoki
{

namespace
{

using ExyokiOffice::Tools::ReportDocument;
using ExyokiOffice::Tools::ReportNode;
using ExyokiOffice::Tools::ToolDiagnostic;
using ExyokiOffice::Tools::ToolSeverity;

/// Bumped whenever the shape of the `commands` payload changes incompatibly.
constexpr int CatalogSchemaVersion = 1;

/// Exit codes any command can return: the usual success, failure, bad usage and
/// crash-guard triple. Outcome-specific codes are listed per command below.
constexpr ExitCode CommonExitCodes[] = {ExitCode::Ok, ExitCode::OperationFailed, ExitCode::UsageError,
                                        ExitCode::UnhandledException};

/// A command and one extra exit code it can return on top of CommonExitCodes.
struct CommandExitCode
{
    const char* CommandPath;
    ExitCode Code;
};

/// The only part of the catalog that is not derived from the parser: which
/// command reports which outcome through which code. AdaptCommands reports a
/// diagnostic when an entry here no longer matches a registered command.
constexpr CommandExitCode CommandExitCodes[] = {
    {"validate", ExitCode::ValidationErrors},
    {"schema", ExitCode::ValidationErrors},
    {"diff", ExitCode::DiffDifferent},
    {"compare", ExitCode::DiffDifferent},
    {"search", ExitCode::SearchNoMatch},
    {"query", ExitCode::QueryNoMatch},
    {"signatures", ExitCode::SignatureInvalid},
};

class CommandCatalogBuilder
{
public:
    ReportNode BuildCommands(const CLI::App& app)
    {
        auto array = ReportNode::MakeArray();
        const auto inherited = app.get_options();
        for (const auto* command : Subcommands(app))
        {
            array.Push(BuildCommand(*command, command->get_name(), inherited));
        }
        return array;
    }

    /// Command options that hide an inherited global one, as "<command> <option>".
    const std::vector<std::string>& ShadowedGlobalOptions() const { return m_shadowed; }

    /// Names in CommandExitCodes that no longer correspond to a registered command.
    std::vector<std::string> StaleExitCodeEntries() const
    {
        std::vector<std::string> stale;
        for (const auto& entry : CommandExitCodes)
        {
            if (std::find(m_paths.begin(), m_paths.end(), entry.CommandPath) == m_paths.end())
            {
                stale.emplace_back(entry.CommandPath);
            }
        }
        return stale;
    }

    static ReportNode BuildGlobalOptions(const CLI::App& app)
    {
        auto array = ReportNode::MakeArray();
        for (const auto* option : VisibleOptions(app))
        {
            array.Push(BuildOption(*option));
        }
        return array;
    }

    static ReportNode BuildExitCodes()
    {
        auto array = ReportNode::MakeArray();
        array.SetTableHint({"code", "name", "meaning"});
        for (const auto& description : AllExitCodes)
        {
            auto node = ReportNode::MakeObject();
            node.Set("code", static_cast<int>(description.Code));
            node.Set("name", std::string(description.Name));
            node.Set("meaning", std::string(description.Meaning));
            array.Push(std::move(node));
        }
        return array;
    }

private:
    /// Subcommands in declaration order, skipping nameless option groups.
    static std::vector<const CLI::App*> Subcommands(const CLI::App& app)
    {
        return app.get_subcommands([](const CLI::App* candidate)
                                   { return !candidate->get_name().empty(); });
    }

    /// Every option of this app that --help would show; hidden options (the ones
    /// with no group) stay out of the catalog exactly as they stay out of --help.
    static std::vector<const CLI::Option*> VisibleOptions(const CLI::App& app)
    {
        std::vector<const CLI::Option*> options;
        for (const auto* option : app.get_options())
        {
            if (!option->get_group().empty())
            {
                options.push_back(option);
            }
        }
        return options;
    }

    ReportNode BuildCommand(const CLI::App& command, const std::string& path,
                            const std::vector<const CLI::Option*>& inherited)
    {
        m_paths.push_back(path);

        auto node = ReportNode::MakeObject();
        node.Set("name", command.get_name());
        node.Set("path", path);
        node.Set("description", command.get_description());

        // CLI11 folds the options of a fallthrough() parent into get_options(), so
        // every subcommand would otherwise repeat --format, --output, --quiet and
        // --version. Those are reported once, as global options. Every command has
        // its own --help object, which is global in the same sense.
        auto own = VisibleOptions(command);
        own.erase(std::remove_if(own.begin(), own.end(),
                                 [&command, &inherited](const CLI::Option* option)
                                 {
                                     return option == command.get_help_ptr() ||
                                            std::find(inherited.begin(), inherited.end(), option) != inherited.end();
                                 }),
                  own.end());

        // A command option spelled like an inherited one wins after the subcommand
        // name while the global still wins before it, so the same spelling would
        // mean two things depending on where it sits. Recorded here rather than
        // left to review; see the --output note in main.cpp.
        for (const auto* option : own)
        {
            if (option->get_positional())
            {
                continue;
            }
            for (const auto* global : inherited)
            {
                if (global->check_name(option->get_name()))
                {
                    m_shadowed.push_back(path + " " + option->get_name());
                }
            }
        }

        const auto subcommands = Subcommands(command);
        node.Set("usage", BuildUsage(command, path, own, !subcommands.empty()));
        node.Set("requiresSubcommand", command.get_require_subcommand_min() > 0);

        auto positionals = ReportNode::MakeArray();
        auto options = ReportNode::MakeArray();
        for (const auto* option : own)
        {
            (option->get_positional() ? positionals : options).Push(BuildOption(*option));
        }
        node.Set("positionals", std::move(positionals));
        node.Set("options", std::move(options));
        node.Set("exitCodes", BuildCommandExitCodes(path));

        if (!subcommands.empty())
        {
            auto children = ReportNode::MakeArray();
            const auto childInherited = command.get_options();
            for (const auto* child : subcommands)
            {
                children.Push(BuildCommand(*child, path + " " + child->get_name(), childInherited));
            }
            node.Set("subcommands", std::move(children));
        }

        return node;
    }

    static ReportNode BuildCommandExitCodes(const std::string& path)
    {
        std::vector<ExitCode> codes(std::begin(CommonExitCodes), std::end(CommonExitCodes));
        for (const auto& entry : CommandExitCodes)
        {
            if (path == entry.CommandPath)
            {
                codes.push_back(entry.Code);
            }
        }
        std::sort(codes.begin(), codes.end());

        auto array = ReportNode::MakeArray();
        for (const auto code : codes)
        {
            array.Push(static_cast<int>(code));
        }
        return array;
    }

    static ReportNode BuildOption(const CLI::Option& option)
    {
        auto node = ReportNode::MakeObject();
        const bool positional = option.get_positional();
        node.Set("name", option.get_name());
        node.Set("positional", positional);

        if (!positional)
        {
            auto names = ReportNode::MakeArray();
            for (const auto& name : option.get_snames())
            {
                names.Push("-" + name);
            }
            for (const auto& name : option.get_lnames())
            {
                names.Push("--" + name);
            }
            node.Set("names", std::move(names));
        }

        node.Set("description", option.get_description());
        node.Set("required", option.get_required());
        // A vector-valued option reports a very large maximum; anything above one
        // means the option may be given repeatedly.
        node.Set("repeatable", option.get_expected_max() > 1);

        // CLI11 keeps a flag's value type (bool) but expects zero arguments for it.
        const bool isFlag = option.get_items_expected_max() == 0;
        node.Set("flag", isFlag);
        if (!isFlag)
        {
            const auto typeName = option.get_type_name();
            const auto separator = typeName.find(':');
            node.Set("valueType", typeName.substr(0, separator));
            if (separator != std::string::npos)
            {
                const auto constraint = typeName.substr(separator + 1);
                const auto choices = SplitChoices(constraint);
                if (choices.empty())
                {
                    node.Set("constraint", constraint);
                }
                else
                {
                    auto array = ReportNode::MakeArray();
                    for (const auto& choice : choices)
                    {
                        array.Push(choice);
                    }
                    node.Set("choices", std::move(array));
                }
            }
        }

        const auto defaultValue = option.get_default_str();
        if (!defaultValue.empty())
        {
            node.Set("default", defaultValue);
        }
        return node;
    }

    /// Turns an IsMember validator description ("{a,b,c}") into a list; returns
    /// nothing for any other constraint, which stays a free-form string.
    static std::vector<std::string> SplitChoices(const std::string& constraint)
    {
        if (constraint.size() < 2 || constraint.front() != '{' || constraint.back() != '}')
        {
            return {};
        }

        std::vector<std::string> choices;
        const auto body = constraint.substr(1, constraint.size() - 2);
        std::string::size_type start = 0;
        while (start <= body.size())
        {
            const auto comma = body.find(',', start);
            const auto end = comma == std::string::npos ? body.size() : comma;
            choices.push_back(body.substr(start, end - start));
            if (comma == std::string::npos)
            {
                break;
            }
            start = comma + 1;
        }
        return choices;
    }

    static std::string BuildUsage(const CLI::App& command, const std::string& path,
                                  const std::vector<const CLI::Option*>& own, bool hasSubcommands)
    {
        std::string usage = "exyoki " + path;
        if (hasSubcommands)
        {
            usage += command.get_require_subcommand_min() > 0 ? " <subcommand>" : " [<subcommand>]";
        }
        for (const auto* option : own)
        {
            if (!option->get_positional())
            {
                continue;
            }
            const auto name = "<" + option->get_name() + ">" + (option->get_expected_max() > 1 ? "..." : "");
            usage += option->get_required() ? " " + name : " [" + name + "]";
        }
        const bool hasNamedOptions = std::any_of(own.begin(), own.end(), [](const CLI::Option* option)
                                                 { return !option->get_positional(); });
        if (hasNamedOptions)
        {
            usage += " [options]";
        }
        return usage;
    }

    /// Command paths visited by BuildCommands, used to detect a stale exit code table.
    std::vector<std::string> m_paths;
    std::vector<std::string> m_shadowed;
};

} // namespace

ReportDocument AdaptCommands(const CLI::App& app)
{
    ReportDocument document;
    document.Command = "commands";

    CommandCatalogBuilder builder;
    auto commands = builder.BuildCommands(app);

    document.Data.Set("schemaVersion", CatalogSchemaVersion);
    document.Data.Set("description", app.get_description());
    document.Data.Set("globalOptions", CommandCatalogBuilder::BuildGlobalOptions(app));
    document.Data.Set("exitCodes", CommandCatalogBuilder::BuildExitCodes());
    document.Data.Set("commandCount", static_cast<ExyokiOffice::UInt64>(commands.AsArray().size()));
    document.Data.Set("commands", std::move(commands));

    for (const auto& stale : builder.StaleExitCodeEntries())
    {
        document.Status = "error";
        document.Diagnostics.push_back(
            ToolDiagnostic{ToolSeverity::Error, "Exit code table names a command that no longer exists", stale});
    }

    for (const auto& shadowed : builder.ShadowedGlobalOptions())
    {
        document.Status = "error";
        document.Diagnostics.push_back(ToolDiagnostic{
            ToolSeverity::Error, "Command option shadows a global option of the same name", shadowed});
    }

    return document;
}

} // namespace exyoki
