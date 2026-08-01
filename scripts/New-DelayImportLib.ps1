<#
.SYNOPSIS
Regenerates an MSVC short-format import library from a DLL's export table.

The prebuilt FFmpeg import libraries (C:\ffmpeg\lib\*.lib) are dlltool-style
long-format archives. MSVC's /DELAYLOAD cannot attribute imports from those
to their DLL (LNK4199 "no imports found"), which silently leaves the imports
static. Short-format libraries produced by lib.exe /def work with /DELAYLOAD.
#>
param(
    [Parameter(Mandatory = $true)][string]$DllPath,
    [Parameter(Mandatory = $true)][string]$DumpbinExe,
    [Parameter(Mandatory = $true)][string]$LibExe,
    [Parameter(Mandatory = $true)][string]$OutLib
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $DllPath)) { throw "DLL not found: $DllPath" }
$dllName = Split-Path -Leaf $DllPath

$exports = & $DumpbinExe /NOLOGO /EXPORTS $DllPath
if ($LASTEXITCODE -ne 0) { throw "dumpbin failed on $DllPath (exit $LASTEXITCODE)" }

# Export rows look like: "  ordinal  hint  RVA       name [= forwarder]"
$names = @()
$inTable = $false
foreach ($line in $exports) {
    if ($line -match '^\s+ordinal\s+hint\s+RVA\s+name') { $inTable = $true; continue }
    if (-not $inTable) { continue }
    if ($line -match '^\s*Summary') { break }
    if ($line -match '^\s*\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]{8}\s+(\S+)') {
        $names += $Matches[1]
    }
}
if ($names.Count -eq 0) { throw "No exports parsed from $DllPath" }

$defPath = [System.IO.Path]::ChangeExtension($OutLib, ".def")
$defLines = @("LIBRARY $dllName", "EXPORTS") + $names
Set-Content -LiteralPath $defPath -Value $defLines -Encoding ascii

& $LibExe /NOLOGO "/DEF:$defPath" /MACHINE:X64 "/OUT:$OutLib"
if ($LASTEXITCODE -ne 0) { throw "lib.exe failed for $OutLib (exit $LASTEXITCODE)" }

Write-Host "Generated $OutLib ($($names.Count) exports from $dllName)"
