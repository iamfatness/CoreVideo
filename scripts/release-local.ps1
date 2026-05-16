param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^v\d+\.\d+\.\d+([-.][A-Za-z0-9.]+)?$')]
    [string]$Version,

    [Parameter()]
    [string]$BuildPath,
    [Parameter()]
    [string]$Configuration,
    [Parameter()]
    [string]$InstallDir,
    [Parameter()]
    [string]$DistDir,
    [Parameter()]
    [string]$ZipName,

    [Parameter()]
    [switch]$Configure,
    [Parameter()]
    [string]$Generator,
    [Parameter()]
    [string]$CMakePrefixPath,
    [Parameter()]
    [string]$LibObsDir,
    [Parameter()]
    [string]$ObsFrontendApiDir,
    [Parameter()]
    [string]$QtRootDir,
    [Parameter()]
    [string]$ZoomSdkDir,

    [Parameter()]
    [switch]$SkipBuild,
    [Parameter()]
    [switch]$Upload,
    [Parameter()]
    [switch]$Install,
    [Parameter()]
    [string]$ObsInstallPath
)

$ErrorActionPreference = "Stop"

if (-not $BuildPath) { $BuildPath = "build-nmake" }
if (-not $Configuration) { $Configuration = "Release" }
if (-not $InstallDir) { $InstallDir = "install-local-release" }
if (-not $DistDir) { $DistDir = "dist" }
if (-not $Generator) { $Generator = "NMake Makefiles" }
if (-not $ZoomSdkDir) { $ZoomSdkDir = "third_party/zoom-sdk" }
if (-not $ObsInstallPath) { $ObsInstallPath = "C:\Program Files\obs-studio" }

function Resolve-RepoPath {
    param([string]$Path)
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path (Join-Path $PSScriptRoot "..") $Path))
}

function Remove-RepoDirectory {
    param(
        [string]$Path,
        [string]$RepoRoot
    )
    $resolved = [System.IO.Path]::GetFullPath($Path)
    $root = [System.IO.Path]::GetFullPath($RepoRoot).TrimEnd('\') + '\'
    if (-not $resolved.StartsWith($root, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove path outside repository: $resolved"
    }
    if (Test-Path -LiteralPath $resolved) {
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}

function Invoke-GitHubApi {
    param(
        [ValidateSet("Get", "Post", "Patch", "Delete")]
        [string]$Method,
        [string]$Uri,
        [object]$Body,
        [string]$ContentType = "application/json"
    )

    if (-not $script:GitHubToken) {
        $credLines = "protocol=https`nhost=github.com`n`n" | git credential fill
        $script:GitHubToken = (
            $credLines |
                Where-Object { $_ -like "password=*" } |
                ForEach-Object { $_.Substring(9) } |
                Select-Object -First 1
        )
        if (-not $script:GitHubToken) {
            throw "Could not get a GitHub token from Git Credential Manager. Sign in with Git first, then rerun."
        }
    }

    $headers = @{
        Authorization          = "Bearer $script:GitHubToken"
        Accept                 = "application/vnd.github+json"
        "X-GitHub-Api-Version" = "2022-11-28"
        "User-Agent"           = "CoreVideo-Local-Release"
    }

    if ($PSBoundParameters.ContainsKey("Body")) {
        return Invoke-RestMethod -Method $Method -Uri $Uri -Headers $headers -Body $Body -ContentType $ContentType
    }

    return Invoke-RestMethod -Method $Method -Uri $Uri -Headers $headers
}

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$resolvedBuildPath = Resolve-RepoPath $BuildPath
$installPath = Resolve-RepoPath $InstallDir
$distPath = Resolve-RepoPath $DistDir
$zipFileName = if ($ZipName) { $ZipName } else { "CoreVideo-Windows-x64-$Version.zip" }
$zipPath = Join-Path $distPath $zipFileName

Push-Location $repoRoot
try {
    $branch = (git rev-parse --abbrev-ref HEAD).Trim()
    $head = (git rev-parse HEAD).Trim()
    $status = (git status --porcelain)
    if ($status) {
        Write-Warning "Working tree has uncommitted changes. Release package will be built from the current local state."
    }
    if ($branch -ne "main") {
        Write-Warning "Current branch is '$branch', not 'main'."
    }

    if ($Configure) {
        $configureArgs = @(
            "-B", $resolvedBuildPath,
            "-G", $Generator,
            "-DCMAKE_BUILD_TYPE=$Configuration",
            "-DZOOM_SDK_DIR=$ZoomSdkDir"
        )
        if ($CMakePrefixPath) { $configureArgs += "-DCMAKE_PREFIX_PATH=$CMakePrefixPath" }
        if ($LibObsDir) { $configureArgs += "-DLibObs_DIR=$LibObsDir" }
        if ($ObsFrontendApiDir) { $configureArgs += "-Dobs-frontend-api_DIR=$ObsFrontendApiDir" }
        if ($QtRootDir) {
            if ($CMakePrefixPath) {
                $configureArgs += "-DCMAKE_PREFIX_PATH=$CMakePrefixPath;$QtRootDir"
            } else {
                $configureArgs += "-DCMAKE_PREFIX_PATH=$QtRootDir"
            }
        }
        cmake @configureArgs
    } elseif (-not (Test-Path -LiteralPath $resolvedBuildPath)) {
        throw "Build directory does not exist: $resolvedBuildPath. Rerun with -Configure and the required CMake paths, or provide -BuildPath."
    }

    if (-not $SkipBuild) {
        cmake --build $resolvedBuildPath --config $Configuration --parallel
    }

    Remove-RepoDirectory -Path $installPath -RepoRoot $repoRoot
    New-Item -ItemType Directory -Force -Path $installPath | Out-Null
    cmake --install $resolvedBuildPath --config $Configuration --prefix $installPath

    $required = @(
        "obs-plugins\64bit\obs-zoom-plugin.dll",
        "obs-plugins\64bit\zoom-runtime\ZoomObsEngine.exe",
        "obs-plugins\64bit\zoom-runtime\sdk.dll"
    )
    foreach ($relative in $required) {
        $candidate = Join-Path $installPath $relative
        if (-not (Test-Path -LiteralPath $candidate)) {
            throw "Release package is incomplete. Missing: $relative"
        }
    }

    New-Item -ItemType Directory -Force -Path $distPath | Out-Null
    if (Test-Path -LiteralPath $zipPath) {
        Remove-Item -LiteralPath $zipPath -Force
    }
    Compress-Archive -Path (Join-Path $installPath "*") -DestinationPath $zipPath -Force
    $hash = (Get-FileHash $zipPath -Algorithm SHA256).Hash

    if ($Install) {
        if (-not (Test-Path -LiteralPath $ObsInstallPath)) {
            throw "OBS install path does not exist: $ObsInstallPath"
        }
        Copy-Item -Path (Join-Path $installPath "*") -Destination $ObsInstallPath -Recurse -Force
    }

    if ($Upload) {
        $repo = (git config --get remote.origin.url).Trim()
        if ($repo -notmatch "github\.com[:/](?<owner>[^/]+)/(?<name>[^/.]+)(\.git)?$") {
            throw "Could not infer GitHub repository from remote.origin.url: $repo"
        }
        $repoFullName = "$($Matches.owner)/$($Matches.name)"

        try {
            $release = Invoke-GitHubApi -Method Get -Uri "https://api.github.com/repos/$repoFullName/releases/tags/$Version"
        } catch {
            $body = @{
                tag_name = $Version
                target_commitish = $head
                name = "CoreVideo $Version"
                body = "CoreVideo Windows x64 package. Includes OBS plugin, ZoomObsEngine runtime, and Zoom SDK runtime DLLs.`n`nSHA256: $hash"
                draft = $false
                prerelease = $Version.Contains("-")
                generate_release_notes = $true
            } | ConvertTo-Json -Depth 5
            $release = Invoke-GitHubApi -Method Post -Uri "https://api.github.com/repos/$repoFullName/releases" -Body $body
        }

        foreach ($asset in @($release.assets)) {
            if ($asset.name -eq $zipFileName) {
                Invoke-GitHubApi -Method Delete -Uri "https://api.github.com/repos/$repoFullName/releases/assets/$($asset.id)" | Out-Null
            }
        }

        $uploadBase = $release.upload_url.Split("{")[0]
        $uploadUri = $uploadBase + "?name=" + [uri]::EscapeDataString($zipFileName)
        $headers = @{
            Authorization          = "Bearer $script:GitHubToken"
            Accept                 = "application/vnd.github+json"
            "X-GitHub-Api-Version" = "2022-11-28"
            "User-Agent"           = "CoreVideo-Local-Release"
            "Content-Type"         = "application/zip"
        }
        $asset = Invoke-RestMethod -Method Post -Uri $uploadUri -Headers $headers -InFile $zipPath
        Write-Host "Uploaded: $($asset.browser_download_url)"
    }

    Write-Host "Release ZIP: $zipPath"
    Write-Host "SHA256: $hash"
} finally {
    Pop-Location
}
