param(
    [Parameter(Mandatory = $true)]
    [string]$PackageRoot,

    [Parameter()]
    [switch]$FullRuntime,

    [Parameter()]
    [string]$ExpectedOAuthClientId = $env:ZOOM_EMBED_OAUTH_CLIENT_ID,

    [Parameter()]
    [string]$ExpectedMeetingSdkPublicAppKey = $env:ZOOM_EMBED_MEETING_SDK_PUBLIC_APP_KEY,

    [Parameter()]
    [string[]]$ForbiddenBinaryText = @(
        $env:ZOOM_EMBED_OAUTH_CLIENT_SECRET,
        $env:ZOOM_EMBED_MEETING_SDK_SECRET
    ),

    [Parameter()]
    [string]$ManifestPath
)

$ErrorActionPreference = "Stop"

$root = [System.IO.Path]::GetFullPath($PackageRoot)
if (-not (Test-Path -LiteralPath $root -PathType Container)) {
    throw "Package root does not exist: $root"
}

function Test-RequiredFile {
    param([string]$RelativePath)

    $path = Join-Path $root $RelativePath
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Release package is incomplete. Missing: $RelativePath"
    }
    if ((Get-Item -LiteralPath $path).Length -le 0) {
        throw "Release package is incomplete. Empty file: $RelativePath"
    }
}

$required = @(
    "obs-plugins\64bit\obs-zoom-plugin.dll",
    "obs-plugins\64bit\CoreVideoOAuthCallback.exe",
    "obs-plugins\64bit\Qt6Core.dll",
    "obs-plugins\64bit\Qt6Gui.dll",
    "obs-plugins\64bit\Qt6Network.dll",
    "obs-plugins\64bit\Qt6Widgets.dll",
    "obs-plugins\64bit\plugins\platforms\qwindows.dll",
    "obs-plugins\64bit\plugins\tls\qcertonlybackend.dll",
    "obs-plugins\64bit\plugins\tls\qschannelbackend.dll",
    "data\obs-plugins\obs-zoom-plugin\locale\en-US.ini"
)

foreach ($file in $required) {
    Test-RequiredFile $file
}

function Test-BinaryContainsText {
    param(
        [string]$RelativePath,
        [string]$ExpectedText,
        [string]$Description
    )

    if (-not $ExpectedText) { return }

    $path = Join-Path $root $RelativePath
    $bytes = [System.IO.File]::ReadAllBytes($path)
    $haystack = [System.Text.Encoding]::ASCII.GetString($bytes)
    if (-not $haystack.Contains($ExpectedText)) {
        throw "Release package validation failed. $Description '$ExpectedText' was not found in $RelativePath. The package may have been built with stale Zoom app identity values."
    }
}

Test-BinaryContainsText `
    -RelativePath "obs-plugins\64bit\obs-zoom-plugin.dll" `
    -ExpectedText $ExpectedOAuthClientId `
    -Description "Expected embedded OAuth public client ID"
Test-BinaryContainsText `
    -RelativePath "obs-plugins\64bit\obs-zoom-plugin.dll" `
    -ExpectedText $ExpectedMeetingSdkPublicAppKey `
    -Description "Expected embedded Meeting SDK public app key"

foreach ($forbidden in @($ForbiddenBinaryText | Where-Object { $_ })) {
    $path = Join-Path $root "obs-plugins\64bit\obs-zoom-plugin.dll"
    $bytes = [System.IO.File]::ReadAllBytes($path)
    $haystack = [System.Text.Encoding]::ASCII.GetString($bytes)
    if ($haystack.Contains($forbidden)) {
        throw "Release package validation failed. Forbidden secret-like text was found in obs-zoom-plugin.dll."
    }
}

$ffmpegDlls = @(
    "avcodec-62.dll",
    "avdevice-62.dll",
    "avfilter-11.dll",
    "avformat-62.dll",
    "avutil-60.dll",
    "swresample-6.dll",
    "swscale-9.dll"
)

# The FFmpeg runtime must never sit loose in obs-plugins\64bit: OBS 32.2+
# bundles FFmpeg 8 under identical DLL names, and loose copies there stop
# OBS's own obs-ffmpeg module from loading (breaks OBS outright).
$looseFfmpeg = @(Get-ChildItem -LiteralPath (Join-Path $root "obs-plugins\64bit") -File -ErrorAction Stop |
    Where-Object { $_.Name -match '^(avcodec|avdevice|avfilter|avformat|avutil|swresample|swscale)-\d+\.dll$' })
if ($looseFfmpeg.Count -gt 0) {
    throw "Release package validation failed. FFmpeg DLLs must live in obs-plugins\64bit\corevideo-ffmpeg, not loose in obs-plugins\64bit: $($looseFfmpeg.Name -join ', ')"
}

$ffmpegPresent = $ffmpegDlls | Where-Object {
    Test-Path -LiteralPath (Join-Path $root "obs-plugins\64bit\corevideo-ffmpeg\$_")
}
if ($ffmpegPresent.Count -gt 0 -and $ffmpegPresent.Count -ne $ffmpegDlls.Count) {
    throw "Release package has a partial FFmpeg runtime. Present: $($ffmpegPresent -join ', ')"
}

$sidecarPath = Join-Path $root "obs-plugins\64bit\CoreVideoSidecar.exe"
if (Test-Path -LiteralPath $sidecarPath -PathType Leaf) {
    foreach ($file in @(
        "obs-plugins\64bit\data\templates\1-up.json",
        "obs-plugins\64bit\data\templates\8-up-grid.json",
        "obs-plugins\64bit\data\templates\speaker-screenshare.json",
        "obs-plugins\64bit\data\looks\one-up-clean.json",
        "obs-plugins\64bit\data\looks\eight-up-grid.json",
        "obs-plugins\64bit\data\looks\speaker-screenshare.json"
    )) {
        Test-RequiredFile $file
    }
    foreach ($folder in @(
        "obs-plugins\64bit\data\templates",
        "obs-plugins\64bit\data\looks"
    )) {
        $path = Join-Path $root $folder
        $count = @(Get-ChildItem -LiteralPath $path -Filter *.json -File -ErrorAction Stop).Count
        if ($count -lt 6) {
            throw "Release package is incomplete. Sidecar data folder '$folder' contains too few JSON files ($count)."
        }
    }
}

if ($FullRuntime) {
    foreach ($file in @(
        "obs-plugins\64bit\zoom-runtime\ZoomObsEngine.exe",
        "obs-plugins\64bit\zoom-runtime\sdk.dll",
        "obs-plugins\64bit\zoom-runtime\sdkExt.dll",
        "obs-plugins\64bit\zoom-runtime\zSDK.dll",
        "obs-plugins\64bit\zoom-runtime\zVideoApp.dll",
        "obs-plugins\64bit\zoom-runtime\zVideoUI.dll",
        "obs-plugins\64bit\zoom-runtime\zNet.dll",
        "obs-plugins\64bit\zoom-runtime\libcrypto-3-zm.dll",
        "obs-plugins\64bit\zoom-runtime\libssl-3-zm.dll",
        "obs-plugins\64bit\zoom-runtime\zoom.manifest"
    )) {
        Test-RequiredFile $file
    }

    $zoomRuntime = Join-Path $root "obs-plugins\64bit\zoom-runtime"
    $zoomRuntimeFiles = @(Get-ChildItem -LiteralPath $zoomRuntime -File -ErrorAction Stop)
    if ($zoomRuntimeFiles.Count -lt 50) {
        throw "Release package is incomplete. zoom-runtime contains too few runtime files ($($zoomRuntimeFiles.Count))."
    }
}

if ($ManifestPath) {
    $manifestFullPath = [System.IO.Path]::GetFullPath($ManifestPath)
    $manifestParent = Split-Path -Parent $manifestFullPath
    if ($manifestParent) {
        New-Item -ItemType Directory -Force -Path $manifestParent | Out-Null
    }

    $files = Get-ChildItem -LiteralPath $root -Recurse -File |
        Sort-Object FullName |
        ForEach-Object {
            $relative = $_.FullName.Substring($root.TrimEnd('\').Length + 1)
            [pscustomobject]@{
                path = $relative.Replace('\', '/')
                size = $_.Length
                sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            }
        }

    $manifest = [pscustomobject]@{
        generated_at_utc = (Get-Date).ToUniversalTime().ToString("o")
        package_root = $root
        file_count = @($files).Count
        files = @($files)
    }
    $manifest |
        ConvertTo-Json -Depth 5 |
        Set-Content -LiteralPath $manifestFullPath -Encoding UTF8
    Write-Host "CoreVideo package manifest: $manifestFullPath"
}

Write-Host "CoreVideo package validation passed: $root"
