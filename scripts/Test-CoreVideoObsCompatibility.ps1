param(
    [Parameter(Mandatory=$true)][string]$ObsRoot,
    [Parameter(Mandatory=$true)][string]$PackageRoot,
    [Parameter(Mandatory=$true)][string]$ExpectedObsVersion,
    [switch]$ExpectModern
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($ObsRoot)
$exe = Join-Path $root 'bin\64bit\obs64.exe'
$marker = Join-Path $root '.corevideo-compat-fixture'
if (-not (Test-Path $exe)) { throw 'Missing isolated OBS executable' }
if ((Test-Path (Join-Path $root 'config')) -and -not (Test-Path $marker)) {
    throw 'Refusing to modify an existing OBS configuration without a compatibility-fixture marker.'
}
if (Get-Process obs64 -ErrorAction SilentlyContinue) { throw 'Close other OBS instances before this isolated smoke test.' }
New-Item -ItemType File -Force $marker | Out-Null
New-Item -ItemType File -Force (Join-Path $root 'portable_mode.txt') | Out-Null
Copy-Item (Join-Path $PackageRoot '*') $root -Recurse -Force
$logDir = Join-Path $root 'config\obs-studio\logs'
$before = @(Get-ChildItem $logDir -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName)
$p = Start-Process $exe -WorkingDirectory (Split-Path $exe) -ArgumentList '--portable','--multi','--disable-updater','--disable-shutdown-check','--profile','CoreVideoCompatibility','--collection','CoreVideoCompatibility' -WindowStyle Hidden -PassThru
try {
    $deadline = (Get-Date).AddSeconds(45)
    do {
        Start-Sleep -Milliseconds 500
        $log = Get-ChildItem $logDir -ErrorAction SilentlyContinue | Where-Object { $_.FullName -notin $before } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
        $text = if ($log) { Get-Content $log.FullName -Raw } else { '' }
        if ($text -match 'Registered dock: ZoomTalkbackDock') { break }
        $p.Refresh()
        if ($p.HasExited) { throw "OBS exited before plugin startup: $($p.ExitCode)" }
    } while ((Get-Date) -lt $deadline)
    foreach ($pattern in @("OBS $([regex]::Escape($ExpectedObsVersion)) ", 'Registered CoreVideo source kinds', 'Tiles effect loaded', 'Plugin loaded successfully', 'Registered dock: ZoomControlDock', 'Registered dock: ZoomIsoRecorderDock', 'Registered dock: ZoomOutputManagerDock', 'Registered dock: ZoomDiagnosticsDock', 'Registered dock: ZoomTalkbackDock')) {
        if ($text -notmatch $pattern) { throw "Startup evidence missing: $pattern. Log: $($log.FullName)" }
    }
    if ([regex]::Matches($text, '\[obs-zoom-plugin\] Loading plugin v').Count -ne 1) { throw 'Expected exactly one CoreVideo initialization' }
    if ($ExpectModern -and $text -notmatch "Skipping legacy plugin 'obs-zoom-plugin'.*already loaded from '../../plugins/obs-zoom-plugin/obs-zoom-plugin.dll'") { throw 'Modern entry duplicate suppression was not observed' }
    if ($text -match 'Tiles effect not found|CoreVideo requires OBS|Failed to load.*obs-zoom-plugin') { throw 'CoreVideo load/data error' }
    Write-Output "PASS OBS ${ExpectedObsVersion}: one CoreVideo initialization, source kinds, Tiles shader, five docks. Log: $($log.FullName)"
} finally {
    # Stop only the disposable host we launched; no meeting/auth/start-output commands.
    $owned = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
    if ($owned -and $owned.Path -eq $exe) { Stop-Process -Id $p.Id; $p.WaitForExit(10000) | Out-Null }
}

