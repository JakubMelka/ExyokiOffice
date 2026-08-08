# Copyright (c) 2026 Jakub Melka and Collaborators
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root for full license text.

[CmdletBinding()]
param(
    # RelWithDebInfo by default. The hand-written sources carry no runtime
    # assertions, so a debug build buys only MSVC's iterator debugging and an
    # unoptimized debugger view - and costs roughly six times the running time.
    # Pass `-Configuration Debug` when you want either of those back, which is
    # also what the AddressSanitizer job does.
    [ValidateSet('Debug', 'RelWithDebInfo')]
    [string] $Configuration = 'RelWithDebInfo',

    [ValidateRange(1, 256)]
    [int] $Jobs = [Environment]::ProcessorCount,

    [ValidateSet('None', 'Address')]
    [string] $Sanitizer = 'None',

    [switch] $Clean,

    [switch] $Test,

    # -Test without the areas labelled `slow`, which are the ones that sweep the
    # whole file corpus or the whole schema import rather than one construct.
    # They are about six sevenths of the suite's running time and a few seconds
    # of everything else, so this is the edit-test loop; run the full -Test
    # before calling a change done.
    [switch] $QuickTest,

    [switch] $Install,

    [string] $InstallPrefix = 'build\install'
)

$ErrorActionPreference = 'Stop'

function Invoke-CheckedCommand {
    param(
        [Parameter(Mandatory)]
        [string] $FilePath,

        [Parameter(ValueFromRemainingArguments)]
        [string[]] $ArgumentList
    )

    & $FilePath @ArgumentList
    if ($LASTEXITCODE -ne 0) {
        throw "'$FilePath' failed with exit code $LASTEXITCODE."
    }
}

$vswhereCommand = Get-Command 'vswhere.exe' -ErrorAction SilentlyContinue
if ($null -eq $vswhereCommand) {
    $vswhereFallback = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhereFallback)) {
        throw 'vswhere.exe was not found on PATH or in the Visual Studio Installer directory.'
    }

    $vswherePath = $vswhereFallback
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
$devShell = Join-Path $vsInstall 'Common7\Tools\Launch-VsDevShell.ps1'
$cmakeExe = Join-Path $vsInstall 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ctestExe = Join-Path $vsInstall 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe'

foreach ($requiredPath in @($devShell, $cmakeExe, $ctestExe)) {
    if (-not (Test-Path -LiteralPath $requiredPath)) {
        throw "Required Visual Studio tool was not found: $requiredPath"
    }
}

& $devShell -Arch amd64 -HostArch amd64 -SkipAutomaticLocation

$repositoryRoot = $PSScriptRoot
$previousUseMultiToolTask = $env:UseMultiToolTask
$previousEnforceProcessCount = $env:EnforceProcessCountAcrossBuilds
$previousClMpCount = $env:CL_MPCount
Push-Location $repositoryRoot
try {
    $env:UseMultiToolTask = 'true'
    $env:EnforceProcessCountAcrossBuilds = 'true'
    $env:CL_MPCount = $Jobs.ToString()

    # AddressSanitizer uses a dedicated configure preset and binary directory so
    # that instrumented and uninstrumented objects never share a build tree.
    if ($Sanitizer -eq 'Address') {
        $configurePreset = 'windows-vs-asan'
        $buildSubdirectory = 'build\vs-asan'
        $buildPreset = if ($Configuration -eq 'Debug') { 'asan-debug' } else { 'asan-release' }
    }
    else {
        $configurePreset = 'windows-vs'
        $buildSubdirectory = 'build\vs'
        $buildPreset = if ($Configuration -eq 'Debug') { 'debug' } else { 'release' }
    }

    if ($Clean) {
        $buildDirectory = Join-Path $repositoryRoot $buildSubdirectory
        if (Test-Path -LiteralPath $buildDirectory) {
            Remove-Item -LiteralPath $buildDirectory -Recurse -Force
        }
    }

    Invoke-CheckedCommand -FilePath $cmakeExe -ArgumentList @(
        '--preset',
        $configurePreset,
        "-DEXYOKIOFFICE_BUILD_JOBS=$Jobs"
    )
    Invoke-CheckedCommand -FilePath $cmakeExe -ArgumentList @(
        '--build',
        '--preset',
        $buildPreset,
        '--parallel',
        $Jobs
    )

    if ($Test -or $QuickTest) {
        # The test presets carry a conservative parallel level so that a plain
        # `ctest --preset` is parallel everywhere, CI runners included. Here the
        # machine is known, so the same -Jobs the build used is passed on: the
        # layers are separate processes and every temporary path they write
        # carries the process id, so the only limit is the core count.
        $ctestArguments = @(
            '--preset',
            $buildPreset,
            '--parallel',
            $Jobs
        )
        if ($QuickTest -and -not $Test) {
            $ctestArguments += @('--label-exclude', 'slow')
        }

        Invoke-CheckedCommand -FilePath $ctestExe -ArgumentList $ctestArguments
    }

    if ($Install) {
        $resolvedInstallPrefix = if ([System.IO.Path]::IsPathRooted($InstallPrefix)) {
            $InstallPrefix
        }
        else {
            Join-Path $repositoryRoot $InstallPrefix
        }

        Invoke-CheckedCommand -FilePath $cmakeExe -ArgumentList @(
            '--install',
            (Join-Path $repositoryRoot $buildSubdirectory),
            '--config',
            $Configuration,
            '--prefix',
            $resolvedInstallPrefix
        )

        $installedTool = Join-Path $resolvedInstallPrefix 'bin\exyoki.exe'
        if (-not (Test-Path -LiteralPath $installedTool -PathType Leaf)) {
            throw "Installation completed without the expected exyoki executable: $installedTool"
        }
    }
}
finally {
    $env:UseMultiToolTask = $previousUseMultiToolTask
    $env:EnforceProcessCountAcrossBuilds = $previousEnforceProcessCount
    $env:CL_MPCount = $previousClMpCount
    Pop-Location
}
