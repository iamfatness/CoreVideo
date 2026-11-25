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
    "plugins\obs-zoom-plugin\obs-zoom-plugin.dll",
    "plugins\obs-zoom-plugin\CoreVideoOAuthCallback.exe",
    "plugins\obs-zoom-plugin\Qt6Core.dll",
    "plugins\obs-zoom-plugin\Qt6Gui.dll",
    "plugins\obs-zoom-plugin\Qt6Network.dll",
    "plugins\obs-zoom-plugin\Qt6Widgets.dll",
    "plugins\obs-zoom-plugin\plugins\platforms\qwindows.dll",
    "plugins\obs-zoom-plugin\plugins\tls\qcertonlybackend.dll",
    "plugins\obs-zoom-plugin\plugins\tls\qschannelbackend.dll",
    "plugins\obs-zoom-plugin\data\locale\en-US.ini",
    # The Tiles wall's shader. It ships via the install(DIRECTORY data/ ...)
    # rule, the same way the locale file above does, and until now nothing
    # verified it — which is how a package missing it would reach an operator.
    # A Tiles source whose effect is absent cannot draw at all (see
    # src/zoom-tiles-effect-policy.h), so a missing file here must fail the
    # build rather than be discovered on air. This only proves the file
    # exists, though — it says nothing about whether its contents are
    # current; an existing-but-stale effect file passes this check and would
    # only show up as a degraded wall at runtime, not a build failure.
    "plugins\obs-zoom-plugin\data\effects\corevideo-tiles.effect",
    # Shipped beside every managed FFmpeg (the macOS zip carries no license).
    "plugins\obs-zoom-plugin\data\ffmpeg\LICENSE-GPLv3.txt"
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
    -RelativePath "plugins\obs-zoom-plugin\obs-zoom-plugin.dll" `
    -ExpectedText $ExpectedOAuthClientId `
    -Description "Expected embedded OAuth public client ID"
Test-BinaryContainsText `
    -RelativePath "plugins\obs-zoom-plugin\obs-zoom-plugin.dll" `
    -ExpectedText $ExpectedMeetingSdkPublicAppKey `
    -Description "Expected embedded Meeting SDK public app key"

foreach ($forbidden in @($ForbiddenBinaryText | Where-Object { $_ })) {
    $path = Join-Path $root "plugins\obs-zoom-plugin\obs-zoom-plugin.dll"
    $bytes = [System.IO.File]::ReadAllBytes($path)
    $haystack = [System.Text.Encoding]::ASCII.GetString($bytes)
    if ($haystack.Contains($forbidden)) {
        throw "Release package validation failed. Forbidden secret-like text was found in obs-zoom-plugin.dll."
    }
}

# The runtime ships renamed (cv* — see New-PrivateFfmpeg.ps1) so it can
# never collide with the FFmpeg OBS bundles under the standard names.
$ffmpegDlls = @(
    "cvcodec-62.dll",
    "cvdevice-62.dll",
    "cvfilter-11.dll",
    "cvformat-62.dll",
    "cvutil-60.dll",
    "cvresample-6.dll",
    "cvscale-9.dll"
)

# No FFmpeg runtime may sit loose in plugins\obs-zoom-plugin: OBS 32.2+ bundles
# FFmpeg 8 under the standard names, and loose copies there stop OBS's own
# obs-ffmpeg module from loading (breaks OBS outright). Original-named DLLs
# must not appear anywhere in the package at all.
$looseFfmpeg = @(Get-ChildItem -LiteralPath (Join-Path $root "plugins\obs-zoom-plugin") -File -ErrorAction Stop |
    Where-Object { $_.Name -match '^(avcodec|avdevice|avfilter|avformat|avutil|swresample|swscale|cvcodec|cvdevice|cvfilter|cvformat|cvutil|cvresample|cvscale)-\d+\.dll$' })
if ($looseFfmpeg.Count -gt 0) {
    throw "Release package validation failed. FFmpeg DLLs must live in plugins\obs-zoom-plugin\corevideo-ffmpeg, not loose in plugins\obs-zoom-plugin: $($looseFfmpeg.Name -join ', ')"
}
$originalNamed = @(Get-ChildItem -LiteralPath (Join-Path $root "plugins\obs-zoom-plugin") -Recurse -File -ErrorAction Stop |
    Where-Object { $_.Name -match '^(avcodec|avdevice|avfilter|avformat|avutil|swresample|swscale)-\d+\.dll$' })
if ($originalNamed.Count -gt 0) {
    throw "Release package validation failed. Original-named FFmpeg DLLs found (must be cv*-renamed): $($originalNamed.FullName -join ', ')"
}

$ffmpegPresent = $ffmpegDlls | Where-Object {
    Test-Path -LiteralPath (Join-Path $root "plugins\obs-zoom-plugin\corevideo-ffmpeg\$_")
}
if ($ffmpegPresent.Count -gt 0 -and $ffmpegPresent.Count -ne $ffmpegDlls.Count) {
    throw "Release package has a partial FFmpeg runtime. Present: $($ffmpegPresent -join ', ')"
}

$sidecarPath = Join-Path $root "plugins\obs-zoom-plugin\CoreVideoSidecar.exe"
if (Test-Path -LiteralPath $sidecarPath -PathType Leaf) {
    foreach ($file in @(
        "plugins\obs-zoom-plugin\data\templates\1-up.json",
        "plugins\obs-zoom-plugin\data\templates\8-up-grid.json",
        "plugins\obs-zoom-plugin\data\templates\speaker-screenshare.json",
        "plugins\obs-zoom-plugin\data\looks\one-up-clean.json",
        "plugins\obs-zoom-plugin\data\looks\eight-up-grid.json",
        "plugins\obs-zoom-plugin\data\looks\speaker-screenshare.json"
    )) {
        Test-RequiredFile $file
    }
    foreach ($folder in @(
        "plugins\obs-zoom-plugin\data\templates",
        "plugins\obs-zoom-plugin\data\looks"
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
        "plugins\obs-zoom-plugin\zoom-runtime\ZoomObsEngine.exe",
        "plugins\obs-zoom-plugin\zoom-runtime\sdk.dll",
        "plugins\obs-zoom-plugin\zoom-runtime\sdkExt.dll",
        "plugins\obs-zoom-plugin\zoom-runtime\zSDK.dll",
        "plugins\obs-zoom-plugin\zoom-runtime\zVideoApp.dll",
        "plugins\obs-zoom-plugin\zoom-runtime\zVideoUI.dll",
        "plugins\obs-zoom-plugin\zoom-runtime\zNet.dll",
        "plugins\obs-zoom-plugin\zoom-runtime\libcrypto-3-zm.dll",
        "plugins\obs-zoom-plugin\zoom-runtime\libssl-3-zm.dll",
        "plugins\obs-zoom-plugin\zoom-runtime\zoom.manifest"
    )) {
        Test-RequiredFile $file
    }

    $zoomRuntime = Join-Path $root "plugins\obs-zoom-plugin\zoom-runtime"
    $zoomRuntimeFiles = @(Get-ChildItem -LiteralPath $zoomRuntime -File -ErrorAction Stop)
    if ($zoomRuntimeFiles.Count -lt 50) {
        throw "Release package is incomplete. zoom-runtime contains too few runtime files ($($zoomRuntimeFiles.Count))."
    }
}

# Compatibility entries must be byte-identical to the modern entry.
$primaryHash = (Get-FileHash (Join-Path $root "plugins\obs-zoom-plugin\obs-zoom-plugin.dll")).Hash
foreach ($entry in @("plugins\obs-zoom-plugin\bin\64bit\obs-zoom-plugin.dll", "obs-plugins\64bit\obs-zoom-plugin.dll")) {
    Test-RequiredFile $entry
    if ((Get-FileHash (Join-Path $root $entry)).Hash -ne $primaryHash) { throw "Mismatched compatibility entry: $entry" }
}
Test-RequiredFile "data\obs-plugins\obs-zoom-plugin\locale\en-US.ini"
Test-RequiredFile "data\obs-plugins\obs-zoom-plugin\effects\corevideo-tiles.effect"

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
