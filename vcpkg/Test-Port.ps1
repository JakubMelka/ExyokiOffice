# Copyright (c) 2026 Jakub Melka and Collaborators
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root for full license text.

# Installs the exyokioffice vcpkg port and builds vcpkg/test against it. See
# vcpkg/README.md for where the port lives and how it is released.

[CmdletBinding()]
param(
    [string] $Triplet = 'x64-windows',

    # The vcpkg clone carrying ports/exyokioffice. VCPKG_ROOT wins when set,
    # because that is the same variable the toolchain file and the test project's
    # CMake preset read.
    [string] $VcpkgRoot = $(if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { 'E:\VCPKG\vcpkg' }),

    # Builds the tip of a branch instead of the tagged tarball named by the
    # portfile. Required until the first release tag exists, since the port then
    # has no SHA512 to verify against.
    [switch] $Head,

    # Builds this working tree instead of anything on GitHub, which is how a
    # change to the library or to the port is checked before it is pushed.
    [switch] $LocalSource,

    [ValidateSet('Debug', 'RelWithDebInfo', 'Release')]
    [string] $Configuration = 'RelWithDebInfo',

    [switch] $Clean
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

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$testDirectory = Join-Path $PSScriptRoot 'test'
$buildDirectory = Join-Path $testDirectory 'build\port'
$installRoot = Join-Path $testDirectory 'build\installed'
$overlayRoot = Join-Path $testDirectory 'build\overlay'

$vcpkgExe = Join-Path $VcpkgRoot 'vcpkg.exe'
if (-not (Test-Path -LiteralPath $vcpkgExe)) {
    throw "vcpkg.exe was not found at $vcpkgExe. Pass -VcpkgRoot or set VCPKG_ROOT; see vcpkg/README.md."
}

$portDirectory = Join-Path $VcpkgRoot 'ports\exyokioffice'
if (-not (Test-Path -LiteralPath $portDirectory)) {
    throw "The exyokioffice port was not found at $portDirectory. See vcpkg/README.md."
}

$cmakeCommand = Get-Command 'cmake.exe' -ErrorAction SilentlyContinue
$ctestCommand = Get-Command 'ctest.exe' -ErrorAction SilentlyContinue
if ($null -eq $cmakeCommand -or $null -eq $ctestCommand) {
    $vswhereFallback = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vswherePath = (Get-Command 'vswhere.exe' -ErrorAction SilentlyContinue).Source
    if (-not $vswherePath) {
        if (-not (Test-Path -LiteralPath $vswhereFallback)) {
            throw 'Neither cmake.exe nor vswhere.exe was found; install CMake or Visual Studio.'
        }

        $vswherePath = $vswhereFallback
    }

    $vsInstall = (& $vswherePath -latest -products * -property installationPath).Trim()
    $cmakeExe = Join-Path $vsInstall 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    $ctestExe = Join-Path $vsInstall 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe'
}
else {
    $cmakeExe = $cmakeCommand.Source
    $ctestExe = $ctestCommand.Source
}

foreach ($requiredPath in @($cmakeExe, $ctestExe)) {
    if (-not (Test-Path -LiteralPath $requiredPath)) {
        throw "Required tool was not found: $requiredPath"
    }
}

if ($Clean) {
    foreach ($stalePath in @($buildDirectory, $installRoot, $overlayRoot)) {
        if (Test-Path -LiteralPath $stalePath) {
            Remove-Item -LiteralPath $stalePath -Recurse -Force
        }
    }
}

$installArguments = @(
    'install',
    "exyokioffice[tools,mcp]:$Triplet",
    "--x-install-root=$installRoot"
)

if ($LocalSource) {
    # The published portfile fetches a tagged tarball from GitHub, which is the
    # right thing for a consumer and the wrong thing for checking a change that
    # has not been pushed. The overlay is that same port with only its source
    # acquisition replaced by a copy of this working tree, so everything after
    # it - the options, the install rules, the copyright - is still the port
    # that ships.
    $overlayPort = Join-Path $overlayRoot 'exyokioffice'
    if (Test-Path -LiteralPath $overlayRoot) {
        Remove-Item -LiteralPath $overlayRoot -Recurse -Force
    }

    New-Item -ItemType Directory -Path $overlayPort -Force | Out-Null
    Copy-Item -Path (Join-Path $portDirectory '*') -Destination $overlayPort -Recurse

    $portfilePath = Join-Path $overlayPort 'portfile.cmake'
    $portfile = Get-Content -LiteralPath $portfilePath -Raw
    $repositoryPath = $repositoryRoot -replace '\\', '/'
    $localAcquisition = @"
set(SOURCE_PATH "`${CURRENT_BUILDTREES_DIR}/src/local-source")
file(REMOVE_RECURSE "`${SOURCE_PATH}")
file(COPY "$repositoryPath/" DESTINATION "`${SOURCE_PATH}"
     PATTERN "build" EXCLUDE
     PATTERN ".git" EXCLUDE
     PATTERN ".vs" EXCLUDE)
"@

    $rewritten = [regex]::Replace(
        $portfile,
        '(?s)vcpkg_from_github\(.*?\r?\n\)\r?\n',
        ($localAcquisition -replace '\$', '$$$$') + [Environment]::NewLine)
    if ($rewritten -eq $portfile) {
        throw "Could not rewrite the vcpkg_from_github call in $portfilePath."
    }

    Set-Content -LiteralPath $portfilePath -Value $rewritten -Encoding utf8

    # vcpkg hashes the portfile, not the tree it copies, so a cached package
    # would survive a source change this mode exists to test.
    $installArguments += @("--overlay-ports=$overlayRoot", '--no-binarycaching')
    Write-Host "Building the port from the working tree at $repositoryRoot" -ForegroundColor Cyan
}
elseif ($Head) {
    $installArguments += '--head'
}

Invoke-CheckedCommand -FilePath $vcpkgExe -ArgumentList $installArguments

# Manifest mode is off on purpose: the install above already resolved the
# dependency, with the --head or overlay handling that a manifest cannot express.
Invoke-CheckedCommand -FilePath $cmakeExe -ArgumentList @(
    '-S', $testDirectory,
    '-B', $buildDirectory,
    "-DCMAKE_BUILD_TYPE=$Configuration",
    "-DCMAKE_TOOLCHAIN_FILE=$VcpkgRoot/scripts/buildsystems/vcpkg.cmake",
    "-DVCPKG_TARGET_TRIPLET=$Triplet",
    "-DVCPKG_INSTALLED_DIR=$installRoot",
    '-DVCPKG_MANIFEST_MODE=OFF'
)

Invoke-CheckedCommand -FilePath $cmakeExe -ArgumentList @(
    '--build', $buildDirectory,
    '--config', $Configuration
)

Invoke-CheckedCommand -FilePath $ctestExe -ArgumentList @(
    '--test-dir', $buildDirectory,
    '--build-config', $Configuration,
    '--output-on-failure'
)

Write-Host "The exyokioffice port installs and consumes cleanly on $Triplet." -ForegroundColor Green
