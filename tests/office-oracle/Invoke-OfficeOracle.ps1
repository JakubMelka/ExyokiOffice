# Copyright (c) 2026 Jakub Melka and Contributors
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root for full license text.

<#
.SYNOPSIS
Opens Office documents in Microsoft Office and reports what Office would not keep.

.DESCRIPTION
Schema and package validation prove a document is well formed; they cannot
prove Office accepts it. Every defect this gate exists for was in a file that
validated: a picture with no alternative text Word could read, audio PowerPoint
turned into a broken link on its next save, a conditional format that painted
nothing.

For each .docx, .docm, .xlsx, .xlsm, .pptx and .pptm under -Path, the gate

 1. opens it in its application through COM. PowerPoint refuses a file it would
    have to repair, so a failure to open is a failure. Word and Excel may repair
    without saying so, which is why the second step exists;
 2. has the application save a copy, and compares the package inventory of the
    two. A watched kind of content - media, charts, comments, tables, pivot
    tables, slicers, notes, a VBA project - that is present in the original and
    absent from the copy is content Office dropped, and fails the gate. A count
    that only shrinks is reported as a change, because Office legitimately
    merges some parts.

Each document is handled by a separate PowerShell process with its own Office
instance and a time limit. An application that stops on a dialog nobody can see
would otherwise hang the gate for good; on a timeout the worker, and only the
Office processes started after it, are ended and the document fails.

It needs Microsoft Office on the machine and changes no setting of it. It does
not run macros and does not need trusted access to the VBA object model.

.PARAMETER Path
A document, or a directory searched recursively.

.PARAMETER TimeoutSeconds
How long one document may take to open and save before it fails.

.PARAMETER WorkDirectory
Where the copies Office saves are written. Defaults to a fresh directory under
the temporary directory.

.EXAMPLE
.\.venv\Scripts\python tests\mcp_python\oracle_corpus.py build\oracle
.\tests\office-oracle\Invoke-OfficeOracle.ps1 -Path build\oracle
#>
[CmdletBinding(DefaultParameterSetName = 'Gate')]
param(
    [Parameter(Mandatory = $true, ParameterSetName = 'Gate')]
    [string]$Path,

    [Parameter(ParameterSetName = 'Gate')]
    [int]$TimeoutSeconds = 180,

    [Parameter(ParameterSetName = 'Gate')]
    [string]$WorkDirectory = (Join-Path ([IO.Path]::GetTempPath()) ("exyoki-office-oracle-" + [Guid]::NewGuid().ToString('N'))),

    # Internal: one document, in a process of its own.
    [Parameter(Mandatory = $true, ParameterSetName = 'Worker')]
    [string]$Document,

    [Parameter(Mandatory = $true, ParameterSetName = 'Worker')]
    [string]$CopyDirectory
)

$ErrorActionPreference = 'Stop'

function Release-ComObject($Object)
{
    if ($null -ne $Object)
    {
        [void][Runtime.InteropServices.Marshal]::ReleaseComObject($Object)
    }
}

# Opens $Document read-only in its application, saves a copy into
# $CopyDirectory and returns the copy's path. Opening read-only leaves the
# original exactly as it was.
function Save-OfficeCopy([string]$Document, [string]$CopyDirectory)
{
    $stem = Join-Path $CopyDirectory ([IO.Path]::GetFileNameWithoutExtension($Document))
    $extension = [IO.Path]::GetExtension($Document).ToLowerInvariant()

    if ($extension -in '.docx', '.docm')
    {
        $application = New-Object -ComObject Word.Application
        try
        {
            $application.Visible = $false
            $application.DisplayAlerts = 0 # wdAlertsNone
            $file = $application.Documents.Open($Document, $false, $true, $false)
            try
            {
                if ($extension -eq '.docm') { $copy = "$stem.docm"; $file.SaveAs2($copy, 13) } # wdFormatXMLDocumentMacroEnabled
                else { $copy = "$stem.docx"; $file.SaveAs2($copy, 16) }                         # wdFormatDocumentDefault
            }
            finally
            {
                $file.Close($false)
                Release-ComObject $file
            }
        }
        finally
        {
            $application.Quit()
            Release-ComObject $application
        }

        return $copy
    }

    if ($extension -in '.xlsx', '.xlsm')
    {
        $application = New-Object -ComObject Excel.Application
        try
        {
            $application.Visible = $false
            $application.DisplayAlerts = $false
            $file = $application.Workbooks.Open($Document, 0, $true)
            try
            {
                if ($file.HasVBProject) { $copy = "$stem.xlsm"; $file.SaveAs($copy, 52) } # xlOpenXMLWorkbookMacroEnabled
                else { $copy = "$stem.xlsx"; $file.SaveAs($copy, 51) }                    # xlOpenXMLWorkbook
            }
            finally
            {
                $file.Close($false)
                Release-ComObject $file
            }
        }
        finally
        {
            $application.Quit()
            Release-ComObject $application
        }

        return $copy
    }

    $application = New-Object -ComObject PowerPoint.Application
    try
    {
        # With alerts off, PowerPoint refuses a file it would have to repair
        # instead of repairing it.
        $application.DisplayAlerts = 1 # ppAlertsNone
        $file = $application.Presentations.Open($Document, -1, 0, 0) # read-only, titled, no window
        try
        {
            $copy = "$stem$extension"
            $file.SaveCopyAs($copy)
        }
        finally
        {
            $file.Close()
            Release-ComObject $file
        }
    }
    finally
    {
        $application.Quit()
        Release-ComObject $application
    }

    return $copy
}

if ($PSCmdlet.ParameterSetName -eq 'Worker')
{
    $result = @{ Copy = ''; Error = '' }
    try
    {
        $result.Copy = Save-OfficeCopy $Document $CopyDirectory
    }
    catch
    {
        $result.Error = $_.Exception.Message
    }

    [Console]::Out.WriteLine(($result | ConvertTo-Json -Compress))
    exit 0
}

Add-Type -AssemblyName System.IO.Compression.FileSystem

# Folder or part-name prefixes whose disappearance on an Office save means
# content was dropped rather than reorganized.
$Watched = @('media', 'charts', 'embeddings', 'drawings', 'comments', 'threadedComments', 'persons', 'tables',
             'pivotTables', 'pivotCache', 'slicers', 'slicerCaches', 'notesSlides', 'footnotes', 'endnotes',
             'vbaProject')

function Get-PackageInventory([string]$File)
{
    $counts = [ordered]@{}
    foreach ($kind in $Watched) { $counts[$kind] = 0 }

    $zip = [IO.Compression.ZipFile]::OpenRead($File)
    try
    {
        foreach ($entry in $zip.Entries)
        {
            $name = $entry.FullName
            if ($name.EndsWith('/') -or $name -match '(^|/)_rels/') { continue }

            $segments = $name -split '/'
            $folders = if ($segments.Count -gt 1) { $segments[0..($segments.Count - 2)] } else { @() }
            foreach ($kind in $Watched)
            {
                if (($folders | Where-Object { $_ -ieq $kind }) -or ($segments[-1] -like "$kind*"))
                {
                    $counts[$kind]++
                }
            }
        }
    }
    finally
    {
        $zip.Dispose()
    }

    return $counts
}

function Compare-Inventory($Original, $Copy)
{
    $lost = @()
    $changed = @()
    foreach ($kind in $Watched)
    {
        if ($Original[$kind] -gt 0 -and $Copy[$kind] -eq 0)
        {
            $lost += "$kind ($($Original[$kind]) -> 0)"
        }
        elseif ($Copy[$kind] -lt $Original[$kind])
        {
            $changed += "$kind ($($Original[$kind]) -> $($Copy[$kind]))"
        }
    }

    return @{ Lost = $lost; Changed = $changed }
}

$resolved = (Resolve-Path $Path).Path
$documents = if (Test-Path $resolved -PathType Leaf) { @(Get-Item $resolved) }
             else { @(Get-ChildItem $resolved -Recurse -File) }
$documents = @($documents | Where-Object { $_.Name -match '\.(docx|docm|xlsx|xlsm|pptx|pptm)$' -and
                                            -not $_.Name.StartsWith('~$') })
New-Item -ItemType Directory -Force $WorkDirectory | Out-Null

$script = $MyInvocation.MyCommand.Path
$results = @()
foreach ($file in $documents)
{
    $directory = Join-Path $WorkDirectory ([Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force $directory | Out-Null
    $output = Join-Path $directory 'worker.json'
    $result = [ordered]@{ File = $file.FullName; Opened = $false; Error = ''; Lost = @(); Changed = @() }

    $started = Get-Date
    $worker = Start-Process powershell.exe -PassThru -WindowStyle Hidden -RedirectStandardOutput $output `
        -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$script`"",
                        '-Document', "`"$($file.FullName)`"", '-CopyDirectory', "`"$directory`"")

    if (-not $worker.WaitForExit($TimeoutSeconds * 1000))
    {
        Stop-Process -Id $worker.Id -Force -ErrorAction SilentlyContinue
        # Only Office processes this worker could have started are ended; an
        # instance someone had open before the gate ran is left alone.
        Get-Process WINWORD, EXCEL, POWERPNT -ErrorAction SilentlyContinue |
            Where-Object { $_.StartTime -ge $started } |
            Stop-Process -Force -ErrorAction SilentlyContinue
        $result.Error = "Office did not open and save the document within $TimeoutSeconds seconds."
    }
    else
    {
        $answer = Get-Content $output -Raw -ErrorAction SilentlyContinue
        $reply = if ($answer) { $answer | ConvertFrom-Json } else { $null }
        if ($null -eq $reply) { $result.Error = 'The worker produced no result.' }
        elseif ($reply.Error) { $result.Error = $reply.Error }
        else
        {
            $result.Opened = $true
            $comparison = Compare-Inventory (Get-PackageInventory $file.FullName) (Get-PackageInventory $reply.Copy)
            $result.Lost = $comparison.Lost
            $result.Changed = $comparison.Changed
        }
    }

    $results += [pscustomobject]$result
}

$failed = @($results | Where-Object { -not $_.Opened -or $_.Lost.Count -gt 0 })
foreach ($result in $results)
{
    $verdict = if (-not $result.Opened) { "FAIL  $($result.Error)" }
               elseif ($result.Lost.Count -gt 0) { "FAIL  Office dropped: $($result.Lost -join ', ')" }
               else { 'ok' }
    Write-Output ("{0}  {1}" -f $result.File, $verdict)
    if ($result.Changed.Count -gt 0)
    {
        Write-Output ("    changed: {0}" -f ($result.Changed -join ', '))
    }
}

Write-Output ("{0} document(s), {1} failed." -f $results.Count, $failed.Count)
if ($results.Count -eq 0)
{
    Write-Output "No Office documents were found under $resolved."
    exit 2
}

exit $(if ($failed.Count -gt 0) { 1 } else { 0 })
