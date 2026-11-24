param([Parameter(Mandatory=$true)][string]$PackageRoot)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($PackageRoot).TrimEnd('\')
$legacy = Join-Path $root 'obs-plugins\64bit'
$module = Join-Path $root 'plugins\obs-zoom-plugin'
if (Test-Path -LiteralPath $module) { throw 'Layout conversion requires a fresh CMake staging folder.' }
if (-not (Test-Path -LiteralPath (Join-Path $legacy 'obs-zoom-plugin.dll'))) { throw 'Missing staged plugin.' }
# Every move stays inside the explicitly supplied staging root.
foreach ($path in @($legacy, $module)) {
    if (-not ([IO.Path]::GetFullPath($path).StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase))) { throw 'Invalid staging path' }
}
New-Item -ItemType Directory -Force $module | Out-Null
Get-ChildItem -LiteralPath $legacy | Move-Item -Destination $module
# Merge OBS locale/effects and sidecar data, never nest another module directory.
$data = Join-Path $module 'data'
New-Item -ItemType Directory -Force $data | Out-Null
Copy-Item (Join-Path $root 'data\obs-plugins\obs-zoom-plugin\*') $data -Recurse -Force
# OBS 32 standard and portable entry points share the modern runtime. OBS 33
# loads the modern entry first and skips these same-name copies.
$compat = Join-Path $module 'bin\64bit'
New-Item -ItemType Directory -Force $compat | Out-Null
Copy-Item (Join-Path $module 'obs-zoom-plugin.dll') $compat
Copy-Item (Join-Path $module 'obs-zoom-plugin.dll') $legacy
