# Copyright (c) 2026 Jakub Melka and Collaborators
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root for full license text.

<#
.SYNOPSIS
    Builds and runs the libFuzzer targets on Windows.

.DESCRIPTION
    WinFuzz.ps1 is to fuzzing what WinBuild.ps1 is to building: it locates the
    newest Visual Studio installation with vswhere, loads its Developer
    PowerShell environment, and drives the CMake fuzz presets with the bundled
    CMake. No separate LLVM installation is needed - the libFuzzer runtime ships
    with both the MSVC toolset and the LLVM tools inside Visual Studio.

    The developer environment is required to run the binaries too, not just to
    build them, because the AddressSanitizer runtime DLL lives in the MSVC bin
    directory.

    Crashes are written to tests/fuzz/crashes/<target>/. Commit them: the
    corpus replay test in the unit test suite picks them up automatically, which
    turns every finding into a regression test that runs in an ordinary MSVC
    build.

.PARAMETER Target
    Which fuzz target to run, or 'all' for every target in sequence.

.PARAMETER Toolchain
    Clang uses clang-cl from the Visual Studio LLVM tools (the default and the
    better supported path). MSVC uses cl.exe, whose libFuzzer support Microsoft
    documents as experimental.

.PARAMETER Seconds
    Wall-clock budget per target, passed to libFuzzer as -max_total_time.

.PARAMETER Minimize
    Runs -merge=1 instead of fuzzing, shrinking the corpus to the smallest set
    with the same coverage. Run this before committing corpus growth.

.PARAMETER Replay
    Runs each corpus and crash file once and exits. Fast smoke test of the
    targets themselves; the same inputs also run under ctest.

.EXAMPLE
    .\WinFuzz.ps1 -Target flatopc -Seconds 300

.EXAMPLE
    .\WinFuzz.ps1 -Target all -Toolchain MSVC -Seconds 60
#>

[CmdletBinding()]
param(
    [ValidateSet('flatopc', 'packageload', 'xmlpart', 'simpletypes', 'formula', 'copyops', 'mcprpc', 'all')]
    [string] $Target = 'all',

    [ValidateSet('Clang', 'MSVC')]
    [string] $Toolchain = 'Clang',

    [ValidateRange(1, 604800)]
    [int] $Seconds = 60,

    [ValidateRange(1, 1024)]
    [int] $Jobs = [Environment]::ProcessorCount,

    [switch] $Clean,
    [switch] $Minimize,
    [switch] $Replay,
    [switch] $SkipBuild
)

$ErrorActionPreference = 'Stop'

if ($Minimize -and $Replay) {
    throw '-Minimize and -Replay cannot be used together.'
}

if ($Clean -and $SkipBuild) {
    throw '-Clean and -SkipBuild cannot be used together.'
}

$repoRoot = $PSScriptRoot
$fuzzRoot = Join-Path $repoRoot 'tests\fuzz'

if ($Toolchain -eq 'Clang') {
    $configurePreset = 'windows-ninja-clang-fuzz'
    $buildPreset = 'ninja-clang-fuzz'
    $binaryDir = Join-Path $repoRoot 'build\ninja-clang-fuzz'
}
else {
    $configurePreset = 'windows-ninja-fuzz'
    $buildPreset = 'ninja-fuzz'
    $binaryDir = Join-Path $repoRoot 'build\ninja-fuzz'
}

$allTargets = @('flatopc', 'packageload', 'xmlpart', 'simpletypes', 'formula', 'copyops', 'mcprpc')
$selectedTargets = if ($Target -eq 'all') { $allTargets } else { @($Target) }

# Each target's dictionary; several targets deliberately share one.
$dictionaries = @{
    'flatopc'     = 'flatopc.dict'
    'packageload' = 'zip.dict'
    'xmlpart'     = 'xmlpart.dict'
    'simpletypes' = 'simpletypes.dict'
    'formula'     = 'formula.dict'
    'copyops'     = 'flatopc.dict'
    'mcprpc'      = 'mcprpc.dict'
}

function Find-VisualStudio {
    $vswhereCommand = Get-Command 'vswhere.exe' -ErrorAction SilentlyContinue
    if ($null -eq $vswhereCommand) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (-not (Test-Path -LiteralPath $vswhere)) {
            throw 'vswhere.exe was not found on PATH or in the Visual Studio Installer directory.'
        }
    }
    else {
        $vswhere = $vswhereCommand.Source
    }

    $install = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($install)) {
        throw 'No Visual Studio installation with the MSVC C++ toolchain was found'
    }

    return $install.Trim()
}

function Enter-DeveloperEnvironment {
    param([string] $VsInstall)

    $module = Join-Path $VsInstall 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
    if (-not (Test-Path $module)) {
        throw "Developer PowerShell module not found at $module"
    }

    Import-Module $module
    Enter-VsDevShell -VsInstallPath $VsInstall -SkipAutomaticLocation `
        -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
}

function Get-BundledCMake {
    param([string] $VsInstall)

    $cmake = Join-Path $VsInstall 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if (Test-Path $cmake) {
        return $cmake
    }

    $fallback = Get-Command cmake -ErrorAction SilentlyContinue
    if ($null -eq $fallback) {
        throw 'cmake.exe was not found in Visual Studio nor on PATH'
    }

    return $fallback.Source
}

function Add-ClangRuntimeToPath {
    param([string] $VsInstall)

    # The instrumented binaries - including the source generator, which the
    # build runs - load clang_rt.asan_dynamic-x86_64.dll at start-up. It ships
    # beside the other compiler-rt libraries inside the Visual Studio LLVM
    # tools, a directory the developer environment does not put on PATH, so
    # without this every instrumented process dies with 0xc0000139
    # (STATUS_ENTRYPOINT_NOT_FOUND) before reaching main().
    $clangCl = Join-Path $VsInstall 'VC\Tools\Llvm\x64\bin\clang-cl.exe'
    if (-not (Test-Path $clangCl)) {
        throw "clang-cl.exe not found at $clangCl"
    }

    $resourceDir = & $clangCl -print-resource-dir
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($resourceDir)) {
        throw 'Could not determine the Clang resource directory'
    }

    $runtimeDir = Join-Path $resourceDir.Trim() 'lib\windows'
    if (-not (Test-Path $runtimeDir)) {
        throw "Clang runtime directory not found at $runtimeDir"
    }

    $env:PATH = "$runtimeDir;$env:PATH"
    Write-Host "Clang runtime: $runtimeDir"
}

$vsInstall = Find-VisualStudio
Write-Host "Visual Studio: $vsInstall"
Enter-DeveloperEnvironment -VsInstall $vsInstall
$cmake = Get-BundledCMake -VsInstall $vsInstall

if ($Toolchain -eq 'Clang') {
    Add-ClangRuntimeToPath -VsInstall $vsInstall
}

if ($Clean -and (Test-Path $binaryDir)) {
    Write-Host "Removing $binaryDir"
    Remove-Item -Recurse -Force $binaryDir
}

if (-not $SkipBuild) {
    $env:UseMultiToolTask = 'true'
    $env:EnforceProcessCountAcrossBuilds = 'true'
    $env:CL_MPCount = $Jobs

    Write-Host "Configuring preset $configurePreset"
    & $cmake --preset $configurePreset "-DEXYOKIOFFICE_BUILD_JOBS=$Jobs"
    if ($LASTEXITCODE -ne 0) { throw "cmake --preset $configurePreset failed" }

    Write-Host "Building preset $buildPreset"
    & $cmake --build --preset $buildPreset --parallel $Jobs
    if ($LASTEXITCODE -ne 0) { throw "cmake --build --preset $buildPreset failed" }
}

$failed = @()

foreach ($name in $selectedTargets) {
    $exe = Join-Path $binaryDir "tests\fuzz\fuzz_$name.exe"
    if (-not (Test-Path $exe)) {
        throw "Fuzz binary not found: $exe"
    }

    # Two corpora, and the split matters.
    #
    # libFuzzer writes every input that reaches new coverage into the FIRST
    # directory on its command line, and it does so by the thousand - a one
    # minute run easily adds five thousand files. That directory therefore lives
    # under build/, which is not tracked. Directories listed after it are read
    # only, which is where the small curated seed corpus belongs. Passing the
    # committed corpus first would bury three hand-written seeds under tens of
    # thousands of hash-named files.
    #
    # Use -Minimize to fold the working corpus back down into something worth
    # committing.
    $seedDir = Join-Path $fuzzRoot "corpus\$name"
    $workDir = Join-Path $binaryDir "fuzz-corpus\$name"
    $crashDir = Join-Path $fuzzRoot "crashes\$name"
    foreach ($dir in @($seedDir, $workDir, $crashDir)) {
        if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
    }

    # libFuzzer appends the artifact file name straight onto this prefix, so the
    # trailing separator is what keeps crashes inside the directory.
    $artifactPrefix = "$crashDir\"

    $arguments = @()
    if ($Minimize) {
        # The first merge directory must be empty. Existing files in that
        # directory are always retained, even when redundant.
        $minimizedDir = Join-Path $binaryDir "fuzz-minimized\$name"
        if (Test-Path -LiteralPath $minimizedDir) {
            Remove-Item -LiteralPath $minimizedDir -Recurse -Force
        }
        New-Item -ItemType Directory -Path $minimizedDir | Out-Null

        $arguments += '-merge=1'
        $arguments += $minimizedDir
        $arguments += $seedDir
        $arguments += $workDir
    }
    elseif ($Replay) {
        $arguments += '-runs=0'
        $arguments += $seedDir
        $arguments += $crashDir
    }
    else {
        $arguments += "-max_total_time=$Seconds"
        $arguments += "-artifact_prefix=$artifactPrefix"
        $arguments += '-max_len=65536'
        $arguments += '-rss_limit_mb=2048'
        $arguments += '-malloc_limit_mb=1024'
        $arguments += '-timeout=25'
        # Learns constants from intercepted comparisons; the single biggest win
        # on formats gated by magic values and element names.
        $arguments += '-use_value_profile=1'
        $arguments += '-print_final_stats=1'
        # Order is load bearing: new units land in the first directory, the
        # committed seeds are only read.
        $arguments += $workDir
        $arguments += $seedDir
    }

    $dictName = $dictionaries[$name]
    if ($dictName -and -not $Minimize) {
        $dictPath = Join-Path $fuzzRoot "dictionaries\$dictName"
        if (Test-Path $dictPath) { $arguments += "-dict=$dictPath" }
    }

    Write-Host ''
    Write-Host "=== $name ===" -ForegroundColor Cyan

    # libFuzzer writes its entire progress log to stderr. Windows PowerShell
    # turns native stderr into ErrorRecords, which $ErrorActionPreference='Stop'
    # would treat as a terminating error on the very first status line, so the
    # preference is relaxed for the duration of the run. The exit code stays the
    # signal that matters.
    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $exe @arguments
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousPreference
    }

    if ($exitCode -ne 0) {
        Write-Host "$name exited with $exitCode" -ForegroundColor Red
        $failed += $name
    }
    elseif ($Minimize) {
        $seedBackupDir = Join-Path $binaryDir "fuzz-seed-backup\$name"
        if (Test-Path -LiteralPath $seedBackupDir) {
            Remove-Item -LiteralPath $seedBackupDir -Recurse -Force
        }

        $seedBackupParent = Split-Path -Parent $seedBackupDir
        New-Item -ItemType Directory -Path $seedBackupParent -Force | Out-Null
        Move-Item -LiteralPath $seedDir -Destination $seedBackupDir
        try {
            Move-Item -LiteralPath $minimizedDir -Destination $seedDir
        }
        catch {
            Move-Item -LiteralPath $seedBackupDir -Destination $seedDir
            throw
        }

        Remove-Item -LiteralPath $seedBackupDir -Recurse -Force
    }
}

if ($Minimize) {
    Write-Host ''
    Write-Host "Merged the working corpus into $fuzzRoot\corpus\<target>; review the additions with git status before committing."
}
else {
    Write-Host ''
    Write-Host "Working corpus (not tracked): $binaryDir\fuzz-corpus\<target>"
    Write-Host 'Run with -Minimize to fold its new coverage back into the committed seed corpus.'
}

if ($failed.Count -gt 0) {
    Write-Host ''
    Write-Host "Targets reporting findings: $($failed -join ', ')" -ForegroundColor Red
    Write-Host "Artifacts are in $fuzzRoot\crashes\<target>\; commit them to turn each finding into a regression test."
    exit 1
}

Write-Host ''
Write-Host 'All fuzz targets finished without findings.' -ForegroundColor Green
