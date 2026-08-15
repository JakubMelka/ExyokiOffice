# Copyright (c) 2026 Jakub Melka and Contributors
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root for full license text.

[CmdletBinding()]
param(
    [ValidateSet('All', 'Format', 'Tidy')]
    [string] $Tool = 'All',

    [switch] $Check,

    [switch] $Changed,

    [string] $BaseRef = 'master',

    [ValidateRange(1, 256)]
    [int] $Jobs = [Environment]::ProcessorCount,

    [string] $BuildDirectory = 'build/tidy',

    [switch] $SkipConfigure
)

$ErrorActionPreference = 'Stop'

# Same exclusions as the file filter in .github/workflows/clang-format.yml.
# The nested '.clang-tidy' files in those directories only silence clang-tidy
# checks; clang-format has no per-directory opt-out, and skipping the generated
# trees also keeps clang-tidy from parsing ~180 extra translation units.
$excludedPathPattern = '^(include/ExyokiOffice/DOM/|sources/DOM/|sources/pugixml/|sources/zip/|3rdparty/)'

# Directories whose headers must never produce findings: generated trees, the
# vendored dependencies, and the toolchain. Used both for clang-tidy's own
# header filter and for the output filter below.
$excludedHeaderPattern = '(DOM|pugixml|zip|3rdparty|build|Microsoft Visual Studio|Windows Kits)[/\\]'

# 'Start-Process -ArgumentList' joins the array with single spaces and quotes
# nothing, so an argument containing whitespace (the header filter below holds
# 'Microsoft Visual Studio') would reach the tool torn into several arguments.
function ConvertTo-CommandLineArgument {
    param([Parameter(Mandatory)] [AllowEmptyString()] [string] $Value)

    if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') {
        return $Value
    }

    # Backslashes are escape characters only in front of a quote, so only those
    # runs have to be doubled before the argument is wrapped.
    $escaped = [regex]::Replace($Value, '(\\*)"', '$1$1\"')
    $escaped = [regex]::Replace($escaped, '(\\+)$', '$1$1')

    return '"' + $escaped + '"'
}

# Runs a tool with its output redirected to a file: capturing native stderr with
# '2>&1' would turn every diagnostic line into a PowerShell NativeCommandError.
#
# The native handle is cached before the caller gets a chance to wait, because a
# Process object from 'Start-Process -PassThru' does not own one: once the
# process exits, .NET has nothing left to read the status from and ExitCode
# comes back as $null. Touching Handle makes it keep the handle open. Caching
# can still lose the race against a process that exits immediately, which is why
# Wait-ToolExitCode reports the exit code as unknown rather than as a failure.
function Start-Tool {
    param(
        [Parameter(Mandatory)] [string] $FilePath,
        [Parameter(Mandatory)] [string[]] $ArgumentList,
        [Parameter(Mandatory)] [string] $LogPath
    )

    $quoted = @($ArgumentList | ForEach-Object { ConvertTo-CommandLineArgument $_ })

    $process = Start-Process -FilePath $FilePath -ArgumentList $quoted `
        -NoNewWindow -PassThru -RedirectStandardOutput $LogPath -RedirectStandardError "$LogPath.err"

    try {
        $null = $process.Handle
    }
    catch {
        # The process finished first; Wait-ToolExitCode reports $null for it.
    }

    return $process
}

# Waits for a process started by Start-Tool and returns its exit code, or $null
# when it cannot be determined. Callers must treat $null as "unknown" and fall
# back to the diagnostics parsed out of the tool's log: reading it as a non-zero
# status made every -Check run report a failure, however clean the tree was.
function Wait-ToolExitCode {
    param([Parameter(Mandatory)] [System.Diagnostics.Process] $Process)

    $Process.WaitForExit()

    try {
        return $Process.ExitCode
    }
    catch {
        return $null
    }
}

function Get-ToolOutput {
    param([Parameter(Mandatory)] [string] $LogPath)

    $lines = @()
    foreach ($path in @($LogPath, "$LogPath.err")) {
        if (Test-Path -LiteralPath $path) {
            $lines += @(Get-Content -LiteralPath $path | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
        }
    }

    return $lines
}

# Windows caps a command line near 32 000 characters, so file lists are chunked.
function Split-IntoBatches {
    param(
        [Parameter(Mandatory)] [AllowEmptyCollection()] [string[]] $Items,
        [int] $Size = 40
    )

    $batches = @()
    for ($index = 0; $index -lt $Items.Count; $index += $Size) {
        $batches += , @($Items[$index..([Math]::Min($index + $Size, $Items.Count) - 1)])
    }

    return , $batches
}

$repositoryRoot = $PSScriptRoot
$workRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('winlint-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $workRoot | Out-Null
Push-Location $repositoryRoot
try {
    # --- Toolchain (same discovery as WinBuild.ps1) --------------------------

    $vswhereCommand = Get-Command 'vswhere.exe' -ErrorAction SilentlyContinue
    if ($null -eq $vswhereCommand) {
        $vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (-not (Test-Path -LiteralPath $vswherePath)) {
            throw 'vswhere.exe was not found on PATH or in the Visual Studio Installer directory.'
        }
    }
    else {
        $vswherePath = $vswhereCommand.Source
    }

    $vsInstall = & $vswherePath -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($vsInstall)) {
        throw 'No Visual Studio installation with the MSVC C++ toolchain was found.'
    }

    $vsInstall = $vsInstall.Trim()
    $clangFormatExe = Join-Path $vsInstall 'VC\Tools\Llvm\x64\bin\clang-format.exe'
    $clangTidyExe = Join-Path $vsInstall 'VC\Tools\Llvm\x64\bin\clang-tidy.exe'
    $clangApplyExe = Join-Path $vsInstall 'VC\Tools\Llvm\x64\bin\clang-apply-replacements.exe'
    $devShell = Join-Path $vsInstall 'Common7\Tools\Launch-VsDevShell.ps1'
    $cmakeExe = Join-Path $vsInstall 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    $ninjaExe = Join-Path $vsInstall 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'

    foreach ($requiredPath in @($clangFormatExe, $clangTidyExe, $clangApplyExe, $devShell, $cmakeExe, $ninjaExe)) {
        if (-not (Test-Path -LiteralPath $requiredPath)) {
            throw "Required Visual Studio tool was not found: $requiredPath"
        }
    }

    # --- File selection ------------------------------------------------------

    if ($Changed) {
        $files = @(& git diff --name-only --diff-filter=ACMR "$BaseRef...HEAD") +
        @(& git diff --name-only --diff-filter=ACMR HEAD) +
        @(& git ls-files --others --exclude-standard)
        $files = @($files | Where-Object { $_ -match '\.(cpp|hpp|h)$' -and (Test-Path -LiteralPath $_ -PathType Leaf) })
    }
    else {
        $files = @(& git ls-files '*.cpp' '*.hpp' '*.h')
    }

    # Generated files also live outside the excluded directories (for example
    # sources/Packaging/GeneratedParts.cpp), so the generator banner is checked
    # too. Only the header comment is inspected, so gen/src/Generator.cpp - which
    # emits that banner as a string literal - is still linted.
    $files = @($files |
        Where-Object { $_ -notmatch $excludedPathPattern } |
        Where-Object { ((Get-Content -LiteralPath $_ -TotalCount 6) -join "`n") -notmatch '<auto-generated>' } |
        Sort-Object -Unique)

    if ($files.Count -eq 0) {
        Write-Host 'No source files selected; nothing to do.'
        exit 0
    }

    $mode = if ($Check) { 'check' } else { 'fix' }
    Write-Host "WinLint: tool=$Tool mode=$mode files=$($files.Count) jobs=$Jobs"
    $failed = $false

    # --- clang-format --------------------------------------------------------

    if ($Tool -ne 'Tidy') {
        Write-Host "`n=== clang-format ==="
        $violating = @()

        $batchIndex = 0
        foreach ($batch in (Split-IntoBatches -Items $files)) {
            if ($Check) {
                $logPath = Join-Path $workRoot "format-$batchIndex.log"
                $process = Start-Tool -FilePath $clangFormatExe -LogPath $logPath `
                    -ArgumentList (@('--style=file', '--dry-run', '-Werror') + $batch)
                $exitCode = Wait-ToolExitCode -Process $process

                $reported = @()
                foreach ($line in (Get-ToolOutput -LogPath $logPath)) {
                    if ($line -match '^(.+?):\d+:\d+:\s*(warning|error):\s*code should be clang-formatted') {
                        $violating += $Matches[1]
                        $reported += $Matches[1]
                    }
                }

                # '--dry-run -Werror' exits non-zero for a badly formatted file,
                # which the loop above already recorded. An exit code with no
                # diagnostics behind it means the invocation itself broke - an
                # unreadable file, a malformed .clang-format - and would
                # otherwise pass as a clean batch.
                if ($null -ne $exitCode -and $exitCode -ne 0 -and $reported.Count -eq 0) {
                    Write-Host "clang-format: invocation failed with exit code $exitCode for batch $batchIndex."
                    Get-ToolOutput -LogPath $logPath | ForEach-Object { Write-Host "  $_" }
                    $failed = $true
                }
            }
            else {
                & $clangFormatExe @('--style=file', '-i') @batch
                if ($LASTEXITCODE -ne 0) {
                    throw "clang-format failed with exit code $LASTEXITCODE."
                }
            }

            $batchIndex++
        }

        if ($Check) {
            $violating = @($violating | Sort-Object -Unique)
            Write-Host "clang-format: $($violating.Count) file(s) need formatting."
            $violating | ForEach-Object { Write-Host "  $_" }
            if ($violating.Count -gt 0) {
                $failed = $true
            }
        }
        else {
            Write-Host "clang-format: formatted $($files.Count) file(s) in place."
        }
    }

    # --- clang-tidy ----------------------------------------------------------

    if ($Tool -ne 'Format') {
        Write-Host "`n=== clang-tidy ==="

        $buildPath = if ([System.IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory } else { Join-Path $repositoryRoot $BuildDirectory }
        $compileDatabase = Join-Path $buildPath 'compile_commands.json'

        if (-not $SkipConfigure) {
            # clang-tidy needs a compilation database (the Visual Studio generator
            # does not emit one) and the generated headers on disk, so a dedicated
            # Ninja build directory is configured and only the generator is built.
            & $devShell -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
            & $cmakeExe --preset windows-ninja-clang-debug -B $buildPath -DCMAKE_EXPORT_COMPILE_COMMANDS=ON "-DCMAKE_MAKE_PROGRAM=$ninjaExe"
            if ($LASTEXITCODE -ne 0) {
                throw "CMake configuration failed with exit code $LASTEXITCODE."
            }

            & $cmakeExe --build $buildPath --target generate_openxml --parallel $Jobs
            if ($LASTEXITCODE -ne 0) {
                throw "Building 'generate_openxml' failed with exit code $LASTEXITCODE."
            }
        }

        if (-not (Test-Path -LiteralPath $compileDatabase)) {
            throw "Compilation database was not found: $compileDatabase"
        }

        $databaseFiles = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
        foreach ($entry in (Get-Content -LiteralPath $compileDatabase -Raw | ConvertFrom-Json)) {
            [void] $databaseFiles.Add(($entry.file -replace '\\', '/'))
        }

        # Only translation units can be analyzed; headers are covered through
        # '--header-filter' while they are included from those units.
        $units = @($files |
            Where-Object { $_ -match '\.cpp$' } |
            ForEach-Object { (Join-Path $repositoryRoot $_) -replace '\\', '/' } |
            Where-Object { $databaseFiles.Contains($_) })

        if ($units.Count -eq 0) {
            Write-Host 'clang-tidy: no selected translation unit is present in the compilation database.'
        }
        else {
            Write-Host "clang-tidy: analyzing $($units.Count) translation unit(s)."

            $arguments = @(
                '-p', $buildPath,
                '--quiet',
                '--header-filter=.*',
                "--exclude-header-filter=$excludedHeaderPattern",
                '--extra-arg=-Wno-unused-command-line-argument',
                # The build uses '/WX'. Kept as errors, the first compiler warning
                # in a translation unit ends its analysis, so the checks never run
                # over the rest of it.
                '--extra-arg=-Wno-error'
            )

            $batchSize = [Math]::Max(1, [Math]::Ceiling($units.Count / $Jobs))
            $runs = @()
            $batchIndex = 0
            foreach ($batch in (Split-IntoBatches -Items $units -Size $batchSize)) {
                $batchArguments = $arguments
                if (-not $Check) {
                    # Fixes are exported per batch and applied serially afterwards;
                    # concurrent '--fix' processes would clobber shared headers.
                    $batchArguments += "--export-fixes=$(Join-Path $workRoot "fixes-$batchIndex.yaml")"
                }

                $logPath = Join-Path $workRoot "tidy-$batchIndex.log"
                $runs += [pscustomobject]@{
                    Process = Start-Tool -FilePath $clangTidyExe -ArgumentList ($batchArguments + $batch) -LogPath $logPath
                    Log     = $logPath
                }
                $batchIndex++
            }

            $findings = 0
            $toolErrors = 0
            foreach ($run in $runs) {
                # Nothing here passes '--warnings-as-errors', so a non-zero exit
                # means the invocation itself failed rather than that findings
                # were reported. An unknown exit code is not treated as one:
                # the error lines matched below catch a broken run on their own.
                $exitCode = Wait-ToolExitCode -Process $run.Process
                $runHadToolError = $null -ne $exitCode -and $exitCode -ne 0

                # clang-analyzer reports the sink of a path-sensitive bug wherever
                # it lands, ignoring '--exclude-header-filter', so vendored headers
                # still show up (doctest, pugixml) and are dropped here. A
                # diagnostic owns the indented snippet and note lines that follow
                # it, so the decision is kept until the next unindented line.
                $suppressed = $false
                foreach ($line in (Get-ToolOutput -LogPath $run.Log)) {
                    if ($line -match '^\[\d+/\d+\] Processing file ') {
                        continue
                    }

                    if ($line -match '^(?<file>.+?):\d+:\d+:\s*(warning|error|note):') {
                        $suppressed = $Matches['file'] -match $excludedHeaderPattern
                    }
                    elseif ($line -notmatch '^\s') {
                        $suppressed = $false
                    }

                    if ($suppressed) {
                        continue
                    }

                    Write-Host $line
                    if ($line -match ':\d+:\d+:\s*(warning|error):') {
                        $findings++
                    }
                    elseif ($line -match '^(Error while processing|Error reading configuration|error:)') {
                        # A malformed invocation (unknown option, mangled path)
                        # produces these instead of diagnostics; without counting
                        # them a completely broken run would report zero findings.
                        $runHadToolError = $true
                    }
                }

                if ($runHadToolError) {
                    $toolErrors++
                }
            }

            Write-Host "clang-tidy: $findings finding(s)."
            if ($Check -and $findings -gt 0) {
                $failed = $true
            }

            if ($toolErrors -gt 0) {
                Write-Host "clang-tidy: $toolErrors invocation error(s); see the lines above."
                $failed = $true
            }

            if (-not $Check) {
                $fixFiles = @(Get-ChildItem -LiteralPath $workRoot -Filter 'fixes-*.yaml' | Where-Object { $_.Length -gt 0 })
                if ($fixFiles.Count -eq 0) {
                    Write-Host 'clang-tidy: no automatic fixes were produced.'
                }
                else {
                    Write-Host "clang-tidy: applying fixes from $($fixFiles.Count) batch(es)."
                    & $clangApplyExe $workRoot
                    if ($LASTEXITCODE -ne 0) {
                        throw "clang-apply-replacements failed with exit code $LASTEXITCODE."
                    }

                    # Replacements do not respect .clang-format, so reformat afterwards.
                    foreach ($batch in (Split-IntoBatches -Items $files)) {
                        & $clangFormatExe @('--style=file', '-i') @batch
                        if ($LASTEXITCODE -ne 0) {
                            throw "clang-format failed with exit code $LASTEXITCODE."
                        }
                    }
                }
            }
        }
    }

    if ($failed) {
        Write-Host "`nWinLint: check failed."
        exit 1
    }

    Write-Host "`nWinLint: done."
}
finally {
    Pop-Location
    if (Test-Path -LiteralPath $workRoot) {
        Remove-Item -LiteralPath $workRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
