# Copyright (c) 2026 Jakub Melka and Contributors
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root for full license text.

<#
.SYNOPSIS
    Measures line and region coverage of the library with LLVM source-based
    coverage, using the clang-cl toolchain that ships with Visual Studio.

.DESCRIPTION
    Configures and builds a coverage preset, runs the tests with the raw
    profiles redirected into the output directory, and turns them into a
    summary table and an HTML report.

    The report covers the ExyokiOffice library, not the test executables: what
    it answers is how much of the library a given selection of tests reaches.
    The whole library is in the report whether or not any test calls into it,
    because the coverage build keeps the library shared - see cmake/Coverage.cmake.

    Three modes share that contract: the default layered run (a CTest label
    selection against the instrumented DLL), -Mcdc (the same plus MC/DC), and
    -Monolith (the whole suite and the whole library in one executable, the
    only mode that attributes header code instantiated by tests).

.EXAMPLE
    .\WinCoverage.ps1
    Coverage of the library reached by the unit test layer.

.EXAMPLE
    .\WinCoverage.ps1 -Label word, spreadsheet -LabelExclude slow
    Coverage reached by the Word and Excel layers, without their slow areas.

.EXAMPLE
    .\WinCoverage.ps1 -AllTests
    Coverage reached by the whole suite. Expect this to take a long time.

.EXAMPLE
    .\WinCoverage.ps1 -AllTests -Mcdc
    The same, plus modified condition/decision coverage, into build\coverage-mcdc.
    Builds a second instrumented tree of its own.

.EXAMPLE
    .\WinCoverage.ps1 -Monolith
    The whole suite and the whole library in one executable, into
    build\coverage-monolith. The most exact measurement this script offers:
    no module seams, so inline code in headers is attributed wherever a test
    instantiated it, and MC/DC is always on. The suite runs serially.
#>
[CmdletBinding()]
param(
    # RelWithDebInfo by default. The coverage mapping is emitted by the front
    # end, so an optimized build still attributes every hit to the source range
    # it belongs to; Debug buys sharper per-line counts inside code the
    # optimizer reshaped, at roughly six times the running time.
    [ValidateSet('Debug', 'RelWithDebInfo')]
    [string] $Configuration = 'RelWithDebInfo',

    [ValidateRange(1, 256)]
    [int] $Jobs = [Environment]::ProcessorCount,

    # Also measure modified condition/decision coverage. This builds into a
    # separate tree, because the instrumentation differs, and both the run and
    # the raw profiles cost more than they do without it.
    [switch] $Mcdc,

    # Measure against ExyokiOfficeMonolithTests: the library and every test
    # layer compiled into one executable (see exyokioffice_add_test_monolith
    # in cmake/Tests.cmake). One binary means no attribution seams - header
    # code instantiated only by tests is counted, which the library-scoped
    # report cannot do. Implies MC/DC and the whole suite; the run is one
    # serial process, so -Label does not apply.
    [switch] $Monolith,

    # CTest labels to measure. The default is the unit layer; see
    # docs/Compatibility.md for what the labels select.
    [string[]] $Label = @('unit'),

    # Measure the whole suite instead of $Label.
    [switch] $AllTests,

    [string[]] $LabelExclude = @(),

    # Narrows the report to the hand-written library. Off by default: the
    # generated DOM and packaging code is compiled into the library and shipped
    # with it, so how much of it the suite reaches is a real question about the
    # suite, not noise to be filtered away.
    [switch] $ExcludeGenerated,

    # Also report the toolchain's own headers - the MSVC standard library and
    # the Windows SDK. They carry coverage mappings like any other header, but
    # they are not part of this repository.
    [switch] $IncludeToolchainHeaders,

    [switch] $Clean,

    # Reuse the binaries already in the coverage build tree.
    [switch] $NoBuild,

    [string] $OutputDirectory = 'build\coverage'
)

$ErrorActionPreference = 'Stop'

# The identifier the profile runtime stamps into every raw profile it writes,
# derived for a COFF image from the PDB GUID in its debug directory. Reading it
# from both sides is what lets the run tell which raw profiles came from the
# library and which from a test executable that happened to share the process.
function Get-CoffBinaryId {
    param(
        [Parameter(Mandatory)] [string] $Image,
        [Parameter(Mandatory)] [string] $ReadObjPath
    )

    $debugDirectory = & $ReadObjPath --coff-debug-directory $Image
    if ($LASTEXITCODE -ne 0) {
        return $null
    }

    $match = $debugDirectory | Select-String -Pattern 'PDBGUID:\s*\{([0-9A-Fa-f-]+)\}'
    if (-not $match) {
        return $null
    }

    # ToByteArray() emits the mixed-endian layout a GUID has on disk, which is
    # the byte order the profile runtime hashes, so no manual swapping is needed.
    $guid = [Guid] $match.Matches[0].Groups[1].Value
    return (($guid.ToByteArray() | ForEach-Object { $_.ToString('x2') }) -join '')
}

# The raw profiles a single instrumented module produced. Every file the merge
# pool writes for one module carries that module's signature as its name, so one
# file per signature is enough to identify the whole group.
function Select-ProfilesForBinaryId {
    param(
        [Parameter(Mandatory)] [System.IO.FileInfo[]] $Profiles,
        [Parameter(Mandatory)] [string] $BinaryId,
        [Parameter(Mandatory)] [string] $ProfDataPath
    )

    $selected = @()
    foreach ($group in $Profiles | Group-Object { ($_.BaseName -split '_')[0] }) {
        $shown = & $ProfDataPath show --binary-ids $group.Group[0].FullName
        if ($LASTEXITCODE -ne 0) {
            continue
        }

        if ($shown | Select-String -Pattern "^$BinaryId$" -Quiet) {
            $selected += $group.Group
        }
    }

    return $selected
}

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

# The C++ Clang tools component. It carries clang-cl and the matching
# llvm-profdata and llvm-cov, and the three have to agree: the raw profile
# format is versioned, and a reader from a different LLVM release rejects it.
$llvmBin = Join-Path $vsInstall 'VC\Tools\Llvm\x64\bin'
$clangClExe = Join-Path $llvmBin 'clang-cl.exe'
$profdataExe = Join-Path $llvmBin 'llvm-profdata.exe'
$covExe = Join-Path $llvmBin 'llvm-cov.exe'
$readobjExe = Join-Path $llvmBin 'llvm-readobj.exe'

foreach ($requiredPath in @($devShell, $cmakeExe, $ctestExe)) {
    if (-not (Test-Path -LiteralPath $requiredPath)) {
        throw "Required Visual Studio tool was not found: $requiredPath"
    }
}

foreach ($requiredPath in @($clangClExe, $profdataExe, $covExe, $readobjExe)) {
    if (-not (Test-Path -LiteralPath $requiredPath)) {
        throw ("Required LLVM tool was not found: $requiredPath. " +
            "Install the 'C++ Clang tools for Windows' component of Visual Studio.")
    }
}

& $devShell -Arch amd64 -HostArch amd64 -SkipAutomaticLocation

$repositoryRoot = $PSScriptRoot
Push-Location $repositoryRoot
try {
    # Launch-VsDevShell does not add the LLVM tools, and the preset asks for
    # clang-cl by name rather than by path.
    $env:PATH = "$llvmBin;$env:PATH"

    if ($Monolith) {
        foreach ($unsupported in @('Label', 'LabelExclude', 'AllTests')) {
            if ($PSBoundParameters.ContainsKey($unsupported)) {
                throw ("-$unsupported does not apply to -Monolith: the monolith is one " +
                    'process running the whole suite.')
            }
        }
        if ($Configuration -eq 'Debug') {
            throw 'No Debug monolith preset exists; drop -Configuration Debug.'
        }
        # The monolith preset instruments MC/DC, so the report shows it.
        $Mcdc = $true
        $preset = 'ninja-clang-coverage-monolith'
    }
    else {
        $preset = 'ninja-clang-coverage'
        if ($Mcdc) {
            $preset += '-mcdc'
        }
        if ($Configuration -eq 'Debug') {
            $preset += '-debug'
        }
    }

    $configurePreset = "windows-$preset"
    $buildSubdirectory = "build\$preset"

    $buildDirectory = Join-Path $repositoryRoot $buildSubdirectory

    # Each mode writes beside the others rather than over them, so the reports
    # can be read together. An explicit -OutputDirectory wins.
    if (-not $PSBoundParameters.ContainsKey('OutputDirectory')) {
        if ($Monolith) {
            $OutputDirectory += '-monolith'
        }
        elseif ($Mcdc) {
            $OutputDirectory += '-mcdc'
        }
    }

    $resolvedOutput = if ([System.IO.Path]::IsPathRooted($OutputDirectory)) {
        $OutputDirectory
    }
    else {
        Join-Path $repositoryRoot $OutputDirectory
    }

    if ($Clean) {
        foreach ($staleDirectory in @($buildDirectory, $resolvedOutput)) {
            if (Test-Path -LiteralPath $staleDirectory) {
                Remove-Item -LiteralPath $staleDirectory -Recurse -Force
            }
        }
    }

    if (-not $NoBuild) {
        Invoke-CheckedCommand -FilePath $cmakeExe -ArgumentList @('--preset', $configurePreset)
        Invoke-CheckedCommand -FilePath $cmakeExe -ArgumentList @(
            '--build', '--preset', $preset, '--parallel', $Jobs)
    }

    # What the report is generated against: the library, or in monolith mode
    # the one executable that contains it.
    $coveredBinary = if ($Monolith) {
        Join-Path $buildDirectory 'ExyokiOfficeMonolithTests.exe'
    }
    else {
        Join-Path $buildDirectory 'ExyokiOffice.dll'
    }
    if (-not (Test-Path -LiteralPath $coveredBinary -PathType Leaf)) {
        throw "The instrumented binary was not found: $coveredBinary. Run without -NoBuild."
    }

    # A previous run's raw profiles would be merged into this one, so the
    # directory is emptied rather than added to.
    $profileDirectory = Join-Path $resolvedOutput 'profraw'
    if (Test-Path -LiteralPath $profileDirectory) {
        Remove-Item -LiteralPath $profileDirectory -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $profileDirectory | Out-Null

    # %m expands to a per-module signature, which is needed because the library
    # and each test executable carry their own copy of the profile runtime and
    # write their own profile. The count in front of it asks the runtime to
    # merge into a pool of that many files per module instead of one file per
    # process: a full suite run is hundreds of processes, and one raw profile
    # each - each of them holding a counter for every region of a 60 MB library
    # - would be gigabytes.
    $previousProfileFile = $env:LLVM_PROFILE_FILE
    $env:LLVM_PROFILE_FILE = Join-Path $profileDirectory '%8m.profraw'

    try {
        # Test failures must not stop the run: a report of what the suite did
        # reach is still the thing being asked for, and the failure is visible
        # in the test output above.
        if ($Monolith) {
            # No CTest here: the monolith is meant to run exactly once, in one
            # process, and the layer entries would sweep everything a second
            # time.
            & $coveredBinary
            $testExitCode = $LASTEXITCODE
        }
        else {
            $ctestArguments = @('--preset', $preset, '--parallel', $Jobs)
            if (-not $AllTests) {
                # One alternation rather than one -L per label: repeated -L
                # arguments are combined with AND, and no CTest entry carries
                # two layer labels at once.
                $ctestArguments += @('--label-regex', ('^(' + ($Label -join '|') + ')$'))
            }
            foreach ($excludedLabel in $LabelExclude) {
                $ctestArguments += @('--label-exclude', $excludedLabel)
            }

            & $ctestExe @ctestArguments
            $testExitCode = $LASTEXITCODE
        }
    }
    finally {
        $env:LLVM_PROFILE_FILE = $previousProfileFile
    }

    $rawProfiles = @(Get-ChildItem -LiteralPath $profileDirectory -Filter '*.profraw' -File)
    if ($rawProfiles.Count -eq 0) {
        throw ("No raw profiles were written to $profileDirectory. " +
            'Either the selected labels matched no test, or the build is not instrumented.')
    }

    # Only the covered binary's own raw profiles are merged. In the layered
    # mode the test executables are instrumented too and their profiles sit in
    # the same directory; merging them in makes llvm-cov report "functions
    # have mismatched data" for every inline function two modules compiled
    # differently, and silently drop those functions from the report. In
    # monolith mode there is only one module and the selection is a no-op
    # safeguard against leftovers. Falling back to everything keeps the run
    # working if a future toolchain stops emitting binary IDs.
    $libraryProfiles = @()
    $libraryBinaryId = Get-CoffBinaryId -Image $coveredBinary -ReadObjPath $readobjExe
    if ($libraryBinaryId) {
        $libraryProfiles = @(Select-ProfilesForBinaryId -Profiles $rawProfiles `
                -BinaryId $libraryBinaryId -ProfDataPath $profdataExe)
    }

    if ($libraryProfiles.Count -eq 0) {
        Write-Warning ('Could not tell the covered binary''s raw profiles apart from the ' +
            'others''; merging all of them. Expect llvm-cov to report mismatched ' +
            'functions.')
        $libraryProfiles = $rawProfiles
    }

    $profileData = Join-Path $resolvedOutput 'coverage.profdata'
    Invoke-CheckedCommand -FilePath $profdataExe -ArgumentList (
        @('merge', '-sparse', '-output', $profileData) + $libraryProfiles.FullName)

    # Nothing that belongs to the repository is filtered out by default -
    # vendored components are compiled into the shipped library, and the
    # generated trees are most of it. The alternations accept either separator
    # because llvm-cov matches against the path the compiler recorded, which is
    # not normalized.
    $ignoredPatterns = @()

    if (-not $IncludeToolchainHeaders) {
        $ignoredPatterns += @(
            '[\\/]Microsoft Visual Studio[\\/]',
            '[\\/]Windows Kits[\\/]')
    }

    if ($ExcludeGenerated) {
        # The DOM trees are generated wholesale, so a directory is enough. The
        # Packaging directories are not: the generator writes three translation
        # units into the same folders the hand-written packaging code lives in,
        # so those go by name. The list is the set of files carrying the
        # <auto-generated> banner - OpenXmlPackageFactory.hpp is hand-written
        # even though its .cpp is not.
        $ignoredPatterns += @(
            '[\\/]sources[\\/]DOM[\\/]',
            '[\\/]include[\\/]ExyokiOffice[\\/]DOM[\\/]',
            '[\\/]Packaging[\\/]GeneratedParts\.(cpp|hpp)$',
            '[\\/]Packaging[\\/]GeneratedSchematron\.(cpp|hpp)$',
            '[\\/]Packaging[\\/]OpenXmlPackageFactory\.cpp$')
    }

    $covCommonArguments = @($coveredBinary, "-instr-profile=$profileData")
    foreach ($pattern in $ignoredPatterns) {
        $covCommonArguments += "--ignore-filename-regex=$pattern"
    }

    # The monolith compiles the test sources into the covered binary, so the
    # report has to be pinned to the library's own trees or the tests would
    # pad it. The layered report needs no restriction: the DLL holds nothing
    # but library code.
    $sourceRestriction = @()
    if ($Monolith) {
        $sourceRestriction = @(
            (Join-Path $repositoryRoot 'include'),
            (Join-Path $repositoryRoot 'sources'),
            (Join-Path $repositoryRoot '3rdparty'))
    }

    # llvm-cov omits the MC/DC columns unless asked, even when the profile
    # carries the test vectors.
    $reportArguments = $covCommonArguments
    $showArguments = $covCommonArguments
    if ($Mcdc) {
        $reportArguments += '--show-mcdc-summary'
        $showArguments += '--show-mcdc'
    }
    $reportArguments += $sourceRestriction

    $reportFile = Join-Path $resolvedOutput 'report.txt'
    $report = & $covExe report @reportArguments
    if ($LASTEXITCODE -ne 0) {
        throw "'llvm-cov report' failed with exit code $LASTEXITCODE."
    }
    $report | Out-File -FilePath $reportFile -Encoding utf8

    $htmlDirectory = Join-Path $resolvedOutput 'html'
    Invoke-CheckedCommand -FilePath $covExe -ArgumentList (
        @('show') + $showArguments + @(
            '-format=html',
            "-output-dir=$htmlDirectory",
            '-show-branches=count',
            '-show-line-counts-or-regions',
            '-show-instantiation-summary',
            "-Xdemangler=$(Join-Path $llvmBin 'llvm-cxxfilt.exe')") + $sourceRestriction)

    $report | Write-Output

    Write-Output ''
    Write-Output "Summary:   $reportFile"
    Write-Output "HTML:      $(Join-Path $htmlDirectory 'index.html')"
    Write-Output "Profile:   $profileData"

    if ($testExitCode -ne 0) {
        Write-Warning ("The test run exited with code $testExitCode; the report covers the " +
            'tests that ran, failures included.')
    }
}
finally {
    Pop-Location
}
