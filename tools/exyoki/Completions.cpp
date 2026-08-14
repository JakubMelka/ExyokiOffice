// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "Completions.hpp"

#include <algorithm>
#include <vector>

namespace exyoki
{

/// File-local helpers behind shell completion generation.
class CompletionsHelper
{
public:
    /// Everything the generators need about one top-level command.
    struct CommandWords
    {
        std::string Name;
        std::string Description;
        /// Own long options plus the union of nested subcommand options.
        std::vector<std::string> Options;
        /// Nested subcommand names ("get", "set" for props).
        std::vector<std::string> Subcommands;
    };

    class CompletionModel
    {
    public:
        explicit CompletionModel(const CLI::App& app)
        {
            for (const auto* option : VisibleOptions(app))
            {
                AppendLongNames(*option, m_globalOptions);
            }
            for (const auto* command : Subcommands(app))
            {
                CommandWords words;
                words.Name = command->get_name();
                words.Description = command->get_description();
                CollectOptions(*command, app.get_options(), words.Options);
                for (const auto* child : Subcommands(*command))
                {
                    words.Subcommands.push_back(child->get_name());
                    CollectOptions(*child, command->get_options(), words.Options);
                }
                SortUnique(words.Options);
                m_commands.push_back(std::move(words));
            }
        }

        const std::vector<std::string>& GlobalOptions() const { return m_globalOptions; }
        const std::vector<CommandWords>& Commands() const { return m_commands; }

        std::string CommandNameList() const
        {
            std::string list;
            for (const auto& command : m_commands)
            {
                if (!list.empty())
                {
                    list += ' ';
                }
                list += command.Name;
            }
            return list;
        }

        std::string GlobalOptionList() const { return Join(m_globalOptions); }

        static std::string Join(const std::vector<std::string>& words)
        {
            std::string list;
            for (const auto& word : words)
            {
                if (!list.empty())
                {
                    list += ' ';
                }
                list += word;
            }
            return list;
        }

    private:
        static std::vector<const CLI::App*> Subcommands(const CLI::App& app)
        {
            return app.get_subcommands([](const CLI::App* candidate)
                                       { return !candidate->get_name().empty(); });
        }

        static std::vector<const CLI::Option*> VisibleOptions(const CLI::App& app)
        {
            std::vector<const CLI::Option*> options;
            for (const auto* option : app.get_options())
            {
                if (!option->get_group().empty() && !option->get_positional())
                {
                    options.push_back(option);
                }
            }
            return options;
        }

        static void AppendLongNames(const CLI::Option& option, std::vector<std::string>& names)
        {
            for (const auto& name : option.get_lnames())
            {
                names.push_back("--" + name);
            }
        }

        /// Appends the command's own long options, skipping --help and the options
        /// folded in from the fallthrough() parent (those are global).
        static void CollectOptions(const CLI::App& command, const std::vector<const CLI::Option*>& inherited,
                                   std::vector<std::string>& options)
        {
            for (const auto* option : VisibleOptions(command))
            {
                if (option == command.get_help_ptr() ||
                    std::find(inherited.begin(), inherited.end(), option) != inherited.end())
                {
                    continue;
                }
                AppendLongNames(*option, options);
            }
        }

        static void SortUnique(std::vector<std::string>& words)
        {
            std::sort(words.begin(), words.end());
            words.erase(std::unique(words.begin(), words.end()), words.end());
        }

        std::vector<std::string> m_globalOptions;
        std::vector<CommandWords> m_commands;
    };

    /// Strips characters that would break quoting in generated shell code.
    static std::string SanitizeDescription(const std::string& description)
    {
        std::string sanitized;
        sanitized.reserve(description.size());
        for (const char c : description)
        {
            if (c == '\'' || c == '"' || c == '`' || c == '\\' || c == '\n')
            {
                continue;
            }
            sanitized += c == ':' ? '-' : c;
        }
        return sanitized;
    }

    static std::string GenerateBash(const CompletionModel& model)
    {
        std::string script;
        script += "# bash completion for exyoki. Install with:\n";
        script += "#   exyoki completions bash > /etc/bash_completion.d/exyoki\n";
        script += "# or source it from ~/.bashrc.\n";
        script += "_exyoki_completions()\n";
        script += "{\n";
        script += "    local cur cmd i\n";
        script += "    cur=\"${COMP_WORDS[COMP_CWORD]}\"\n";
        script += "    local global_opts=\"" + model.GlobalOptionList() + "\"\n";
        script += "    local commands=\"" + model.CommandNameList() + "\"\n";
        script += "    cmd=\"\"\n";
        script += "    for ((i=1; i < COMP_CWORD; i++)); do\n";
        script += "        case \"${COMP_WORDS[i]}\" in\n";
        script += "            -*) ;;\n";
        script += "            *) cmd=\"${COMP_WORDS[i]}\"; break ;;\n";
        script += "        esac\n";
        script += "    done\n";
        script += "    if [[ -z \"$cmd\" ]]; then\n";
        script += "        if [[ \"$cur\" == -* ]]; then\n";
        script += "            COMPREPLY=( $(compgen -W \"$global_opts\" -- \"$cur\") )\n";
        script += "        else\n";
        script += "            COMPREPLY=( $(compgen -W \"$commands\" -- \"$cur\") )\n";
        script += "        fi\n";
        script += "        return 0\n";
        script += "    fi\n";
        script += "    local opts=\"\" words=\"\"\n";
        script += "    case \"$cmd\" in\n";
        for (const auto& command : model.Commands())
        {
            script += "        " + command.Name + ") opts=\"" + CompletionModel::Join(command.Options) +
                      "\" words=\"" + CompletionModel::Join(command.Subcommands) + "\" ;;\n";
        }
        script += "    esac\n";
        script += "    if [[ \"$cur\" == -* ]]; then\n";
        script += "        COMPREPLY=( $(compgen -W \"$opts $global_opts\" -- \"$cur\") )\n";
        script += "    else\n";
        script += "        COMPREPLY=( $(compgen -W \"$words\" -f -- \"$cur\") )\n";
        script += "    fi\n";
        script += "    return 0\n";
        script += "}\n";
        script += "complete -o default -F _exyoki_completions exyoki\n";
        return script;
    }

    static std::string GenerateZsh(const CompletionModel& model)
    {
        std::string script;
        script += "#compdef exyoki\n";
        script += "# zsh completion for exyoki. Install by placing this file as _exyoki\n";
        script += "# somewhere on $fpath, e.g.: exyoki completions zsh > ~/.zsh/completions/_exyoki\n";
        script += "_exyoki()\n";
        script += "{\n";
        script += "    local -a commands global_opts opts words\n";
        script += "    commands=(\n";
        for (const auto& command : model.Commands())
        {
            script += "        '" + command.Name + ":" + SanitizeDescription(command.Description) + "'\n";
        }
        script += "    )\n";
        script += "    global_opts=(" + model.GlobalOptionList() + ")\n";
        script += "    if (( CURRENT == 2 )); then\n";
        script += "        _describe -t commands 'exyoki command' commands\n";
        script += "        return\n";
        script += "    fi\n";
        script += "    local cmd=${words[2]}\n";
        script += "    local -a cmd_opts cmd_words\n";
        script += "    case $cmd in\n";
        for (const auto& command : model.Commands())
        {
            script += "        " + command.Name + ") cmd_opts=(" + CompletionModel::Join(command.Options) +
                      "); cmd_words=(" + CompletionModel::Join(command.Subcommands) + ") ;;\n";
        }
        script += "    esac\n";
        script += "    cmd_opts+=($global_opts)\n";
        script += "    if [[ ${words[CURRENT]} == -* ]]; then\n";
        script += "        compadd -- $cmd_opts\n";
        script += "    elif (( ${#cmd_words} )); then\n";
        script += "        compadd -- $cmd_words\n";
        script += "    else\n";
        script += "        _files\n";
        script += "    fi\n";
        script += "}\n";
        script += "_exyoki \"$@\"\n";
        return script;
    }

    static std::string GeneratePowerShell(const CompletionModel& model)
    {
        std::string script;
        script += "# PowerShell completion for exyoki. Install by adding this to your profile:\n";
        script += "#   exyoki completions powershell | Out-String | Invoke-Expression\n";
        script += "Register-ArgumentCompleter -Native -CommandName exyoki -ScriptBlock {\n";
        script += "    param($wordToComplete, $commandAst, $cursorPosition)\n";
        script += "    $commandOptions = @{\n";
        for (const auto& command : model.Commands())
        {
            script += "        '" + command.Name + "' = @(";
            bool first = true;
            for (const auto& word : command.Subcommands)
            {
                script += std::string(first ? "" : ", ") + "'" + word + "'";
                first = false;
            }
            for (const auto& option : command.Options)
            {
                script += std::string(first ? "" : ", ") + "'" + option + "'";
                first = false;
            }
            script += ")\n";
        }
        script += "    }\n";
        script += "    $globalOptions = @(";
        bool firstGlobal = true;
        for (const auto& option : model.GlobalOptions())
        {
            script += std::string(firstGlobal ? "" : ", ") + "'" + option + "'";
            firstGlobal = false;
        }
        script += ")\n";
        script += "    $elements = $commandAst.CommandElements | Select-Object -Skip 1 |\n";
        script += "        ForEach-Object { $_.Extent.Text }\n";
        script += "    $command = $null\n";
        script += "    foreach ($element in $elements) {\n";
        script += "        if ($element -eq $wordToComplete) { continue }\n";
        script += "        if (-not $element.StartsWith('-')) { $command = $element; break }\n";
        script += "    }\n";
        script += "    $candidates = if ($null -eq $command) {\n";
        script += "        @($commandOptions.Keys) + $globalOptions\n";
        script += "    } else {\n";
        script += "        @($commandOptions[$command]) + $globalOptions\n";
        script += "    }\n";
        script += "    $candidates | Where-Object { $_ -and $_ -like \"$wordToComplete*\" } |\n";
        script += "        Sort-Object -Unique | ForEach-Object {\n";
        script += "            [System.Management.Automation.CompletionResult]::new($_, $_, 'ParameterValue', $_)\n";
        script += "        }\n";
        script += "}\n";
        return script;
    }
};

std::string GenerateCompletionScript(const CLI::App& app, std::string_view shell)
{
    const CompletionsHelper::CompletionModel model(app);
    if (shell == "bash")
    {
        return CompletionsHelper::GenerateBash(model);
    }
    if (shell == "zsh")
    {
        return CompletionsHelper::GenerateZsh(model);
    }
    if (shell == "powershell" || shell == "pwsh")
    {
        return CompletionsHelper::GeneratePowerShell(model);
    }
    return {};
}

} // namespace exyoki
