<#
.SYNOPSIS
Produces a collision-proof private copy of the FFmpeg shared runtime by
renaming every DLL to a cv-prefixed name and patching the PE name strings.

OBS bundles FFmpeg under the standard DLL names, and the Windows loader
dedups loaded modules by base name — so a plugin can never keep its own
same-named FFmpeg family isolated from OBS's inside one process. Renaming
gives our runtime globally unique names (the same effect as building FFmpeg
with --build-suffix, without maintaining a source build).

Each family name is renamed by replacing its first two characters with "cv"
(avutil-60.dll → cvutil-60.dll, swscale-9.dll → cvscale-9.dll, ...), which
keeps every string the same length so the import/export tables and their
string references can be patched in place. Exported function names are not
touched. The DLLs are unsigned, so no signature is invalidated.
#>
param(
    [Parameter(Mandatory = $true)][string]$FfmpegBinDir,
    [Parameter(Mandatory = $true)][string]$OutDir
)

$ErrorActionPreference = "Stop"

$families = "avcodec", "avdevice", "avfilter", "avformat", "avutil",
            "swresample", "swscale"
$familyPattern = "^(" + ($families -join "|") + ")-\d+\.dll$"

$dlls = @(Get-ChildItem -LiteralPath $FfmpegBinDir -File |
    Where-Object { $_.Name -match $familyPattern })
if ($dlls.Count -eq 0) { throw "No FFmpeg family DLLs found in $FfmpegBinDir" }

# Old name -> new name, e.g. avfilter-11.dll -> cvfilter-11.dll (same length).
$renames = @{}
foreach ($dll in $dlls) {
    $renames[$dll.Name] = "cv" + $dll.Name.Substring(2)
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# Latin-1 round-trips all 256 byte values, so string replacement on the
# decoded bytes is an exact same-length binary patch.
$latin1 = [System.Text.Encoding]::GetEncoding(28591)

foreach ($dll in $dlls) {
    $bytes = [System.IO.File]::ReadAllBytes($dll.FullName)
    $text = $latin1.GetString($bytes)
    $patched = 0
    foreach ($pair in $renames.GetEnumerator()) {
        $before = $text.Length
        $text = $text.Replace($pair.Key, $pair.Value)
        if ($text.Length -ne $before) { throw "Length changed patching $($dll.Name)" }
        if ($text.Contains($pair.Value)) { $patched++ }
    }
    if (-not $text.Contains($renames[$dll.Name])) {
        throw "Patch failed: $($dll.Name) does not reference its own new name"
    }
    $outPath = Join-Path $OutDir $renames[$dll.Name]
    [System.IO.File]::WriteAllBytes($outPath, $latin1.GetBytes($text))
    Write-Host "Patched $($dll.Name) -> $($renames[$dll.Name])"
}

# Sanity: the renamed runtime must actually load, and its dependencies must
# resolve inside $OutDir (not to any av*-named module). A load failure here
# means the patch is wrong — fail the build, not the user's OBS.
$avfilterNew = $renames.Keys | Where-Object { $_ -like "avfilter-*" } |
    ForEach-Object { $renames[$_] } | Select-Object -First 1
Add-Type -Namespace CvNative -Name Loader -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("kernel32", SetLastError = true, CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern System.IntPtr LoadLibraryExW(string path, System.IntPtr file, uint flags);
[System.Runtime.InteropServices.DllImport("kernel32", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern System.IntPtr GetModuleHandleW(string name);
'@
$LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR = 0x100
$LOAD_LIBRARY_SEARCH_DEFAULT_DIRS = 0x1000
$handle = [CvNative.Loader]::LoadLibraryExW(
    (Join-Path $OutDir $avfilterNew), [IntPtr]::Zero,
    $LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR -bor $LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)
if ($handle -eq [IntPtr]::Zero) {
    throw "Renamed runtime failed to load: $avfilterNew (error $([System.Runtime.InteropServices.Marshal]::GetLastWin32Error()))"
}
foreach ($old in $renames.Keys) {
    if ([CvNative.Loader]::GetModuleHandleW($old) -ne [IntPtr]::Zero) {
        throw "Isolation broken: original-named module $old got loaded"
    }
}
Write-Host "Load check passed: $avfilterNew loads with no av*/sw*-named modules resident"

$license = Join-Path (Split-Path -Parent $FfmpegBinDir) "LICENSE.txt"
if (Test-Path -LiteralPath $license) {
    Copy-Item -LiteralPath $license (Join-Path $OutDir "FFMPEG-LICENSE.txt") -Force
}

Write-Host "Private FFmpeg runtime ready in $OutDir"
