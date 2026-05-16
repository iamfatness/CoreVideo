param(
    [Parameter()]
    [string]$HostName = "127.0.0.1",

    [Parameter()]
    [int]$Port = 4455,

    [Parameter()]
    [string]$Password = $env:OBS_WEBSOCKET_PASSWORD,

    [Parameter()]
    [string]$SceneName = "CoreVideo Smoke Test",

    [Parameter()]
    [ValidateRange(1, 8)]
    [int]$ParticipantCount = 4,

    [Parameter()]
    [int]$TimeoutSeconds = 10,

    [Parameter()]
    [switch]$AuditOnly
)

$ErrorActionPreference = "Stop"

function ConvertTo-ObsAuth {
    param(
        [string]$Password,
        [string]$Salt,
        [string]$Challenge
    )

    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $secretBytes = [System.Text.Encoding]::UTF8.GetBytes($Password + $Salt)
        $secret = [Convert]::ToBase64String($sha.ComputeHash($secretBytes))
        $authBytes = [System.Text.Encoding]::UTF8.GetBytes($secret + $Challenge)
        return [Convert]::ToBase64String($sha.ComputeHash($authBytes))
    } finally {
        $sha.Dispose()
    }
}

function Send-ObsJson {
    param(
        [System.Net.WebSockets.ClientWebSocket]$Socket,
        [hashtable]$Message
    )

    $json = ($Message | ConvertTo-Json -Depth 20 -Compress)
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($json)
    $segment = [ArraySegment[byte]]::new($bytes)
    $Socket.SendAsync($segment, [System.Net.WebSockets.WebSocketMessageType]::Text, $true, [Threading.CancellationToken]::None).GetAwaiter().GetResult() | Out-Null
}

function Receive-ObsJson {
    param(
        [System.Net.WebSockets.ClientWebSocket]$Socket,
        [int]$TimeoutSeconds
    )

    $deadline = [DateTimeOffset]::UtcNow.AddSeconds($TimeoutSeconds)
    $buffer = [byte[]]::new(65536)
    $chunks = New-Object System.Collections.Generic.List[byte]

    while ([DateTimeOffset]::UtcNow -lt $deadline) {
        $remaining = [Math]::Max(1, [int]($deadline - [DateTimeOffset]::UtcNow).TotalMilliseconds)
        $cts = [Threading.CancellationTokenSource]::new($remaining)
        try {
            $result = $Socket.ReceiveAsync([ArraySegment[byte]]::new($buffer), $cts.Token).GetAwaiter().GetResult()
        } catch [System.OperationCanceledException] {
            continue
        } finally {
            $cts.Dispose()
        }

        if ($result.MessageType -eq [System.Net.WebSockets.WebSocketMessageType]::Close) {
            throw "OBS websocket closed while waiting for a response."
        }

        for ($i = 0; $i -lt $result.Count; ++$i) {
            $chunks.Add($buffer[$i])
        }

        if ($result.EndOfMessage) {
            $json = [System.Text.Encoding]::UTF8.GetString($chunks.ToArray())
            return $json | ConvertFrom-Json
        }
    }

    throw "Timed out waiting for OBS websocket response after $TimeoutSeconds second(s)."
}

function Receive-ObsOp {
    param(
        [System.Net.WebSockets.ClientWebSocket]$Socket,
        [int]$Op,
        [int]$TimeoutSeconds,
        [string]$RequestId
    )

    $deadline = [DateTimeOffset]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTimeOffset]::UtcNow -lt $deadline) {
        $remaining = [Math]::Max(1, [int]($deadline - [DateTimeOffset]::UtcNow).TotalSeconds)
        $msg = Receive-ObsJson -Socket $Socket -TimeoutSeconds $remaining
        if ($msg.op -ne $Op) {
            continue
        }
        if ($RequestId -and $msg.d.requestId -ne $RequestId) {
            continue
        }
        return $msg
    }

    if ($RequestId) {
        throw "Timed out waiting for OBS op $Op with request id '$RequestId'."
    }
    throw "Timed out waiting for OBS op $Op."
}

function Invoke-ObsRequest {
    param(
        [System.Net.WebSockets.ClientWebSocket]$Socket,
        [string]$RequestType,
        [hashtable]$RequestData = @{},
        [int]$TimeoutSeconds
    )

    $requestId = [Guid]::NewGuid().ToString("N")
    Send-ObsJson -Socket $Socket -Message @{
        op = 6
        d = @{
            requestType = $RequestType
            requestId = $requestId
            requestData = $RequestData
        }
    }
    $response = Receive-ObsOp -Socket $Socket -Op 7 -TimeoutSeconds $TimeoutSeconds -RequestId $requestId
    if (-not $response.d.requestStatus.result) {
        $comment = $response.d.requestStatus.comment
        if (-not $comment) { $comment = "unknown OBS request failure" }
        throw "$RequestType failed: $comment"
    }
    return $response.d.responseData
}

function Invoke-ObsRequestIfNeeded {
    param(
        [scriptblock]$AlreadyDone,
        [scriptblock]$Request
    )

    if (& $AlreadyDone) {
        return
    }
    & $Request | Out-Null
}

function Get-SceneNames {
    param([System.Net.WebSockets.ClientWebSocket]$Socket)
    $data = Invoke-ObsRequest -Socket $Socket -RequestType "GetSceneList" -TimeoutSeconds $TimeoutSeconds
    return @($data.scenes | ForEach-Object { $_.sceneName })
}

function Get-InputNames {
    param([System.Net.WebSockets.ClientWebSocket]$Socket)
    $data = Invoke-ObsRequest -Socket $Socket -RequestType "GetInputList" -TimeoutSeconds $TimeoutSeconds
    return @($data.inputs | ForEach-Object { $_.inputName })
}

function Get-SceneItems {
    param(
        [System.Net.WebSockets.ClientWebSocket]$Socket,
        [string]$SceneName
    )

    $data = Invoke-ObsRequest -Socket $Socket -RequestType "GetSceneItemList" -RequestData @{ sceneName = $SceneName } -TimeoutSeconds $TimeoutSeconds
    return @($data.sceneItems)
}

function Ensure-Scene {
    param(
        [System.Net.WebSockets.ClientWebSocket]$Socket,
        [string]$Name
    )

    Invoke-ObsRequestIfNeeded `
        -AlreadyDone { (Get-SceneNames -Socket $Socket) -contains $Name } `
        -Request { Invoke-ObsRequest -Socket $Socket -RequestType "CreateScene" -RequestData @{ sceneName = $Name } -TimeoutSeconds $TimeoutSeconds }
}

function Ensure-ColorInput {
    param(
        [System.Net.WebSockets.ClientWebSocket]$Socket,
        [string]$SceneName,
        [string]$InputName,
        [double]$Color
    )

    Invoke-ObsRequestIfNeeded `
        -AlreadyDone { (Get-InputNames -Socket $Socket) -contains $InputName } `
        -Request {
            Invoke-ObsRequest -Socket $Socket -RequestType "CreateInput" -RequestData @{
                sceneName = $SceneName
                inputName = $InputName
                inputKind = "color_source_v3"
                inputSettings = @{ color = $Color }
                sceneItemEnabled = $true
            } -TimeoutSeconds $TimeoutSeconds
        }
}

function Ensure-SceneItem {
    param(
        [System.Net.WebSockets.ClientWebSocket]$Socket,
        [string]$SceneName,
        [string]$SourceName
    )

    $items = Get-SceneItems -Socket $Socket -SceneName $SceneName
    $existing = @($items | Where-Object { $_.sourceName -eq $SourceName } | Select-Object -First 1)
    if ($existing.Count -gt 0) {
        return [int]$existing[0].sceneItemId
    }

    Invoke-ObsRequest -Socket $Socket -RequestType "CreateSceneItem" -RequestData @{
        sceneName = $SceneName
        sourceName = $SourceName
        sceneItemEnabled = $true
    } -TimeoutSeconds $TimeoutSeconds | Out-Null

    $items = Get-SceneItems -Socket $Socket -SceneName $SceneName
    $created = @($items | Where-Object { $_.sourceName -eq $SourceName } | Select-Object -First 1)
    if ($created.Count -eq 0) {
        throw "Created scene item '$SourceName' was not found in scene '$SceneName'."
    }
    return [int]$created[0].sceneItemId
}

function Set-SceneItemBounds {
    param(
        [System.Net.WebSockets.ClientWebSocket]$Socket,
        [string]$SceneName,
        [int]$SceneItemId,
        [double]$X,
        [double]$Y,
        [double]$Width,
        [double]$Height,
        [int]$Index
    )

    Invoke-ObsRequest -Socket $Socket -RequestType "SetSceneItemTransform" -RequestData @{
        sceneName = $SceneName
        sceneItemId = $SceneItemId
        sceneItemTransform = @{
            positionX = $X
            positionY = $Y
            boundsWidth = $Width
            boundsHeight = $Height
            boundsType = "OBS_BOUNDS_STRETCH"
            alignment = 5
        }
    } -TimeoutSeconds $TimeoutSeconds | Out-Null

    Invoke-ObsRequest -Socket $Socket -RequestType "SetSceneItemIndex" -RequestData @{
        sceneName = $SceneName
        sceneItemId = $SceneItemId
        sceneItemIndex = $Index
    } -TimeoutSeconds $TimeoutSeconds | Out-Null
}

function Assert-Contains {
    param(
        [string[]]$Actual,
        [string[]]$Expected,
        [string]$Label
    )

    $missing = @($Expected | Where-Object { $Actual -notcontains $_ })
    if ($missing.Count -gt 0) {
        throw "$Label missing: $($missing -join ', ')"
    }
}

$socket = [System.Net.WebSockets.ClientWebSocket]::new()
try {
    $uri = [Uri]::new("ws://$HostName`:$Port")
    Write-Host "Connecting to OBS websocket at $uri"
    $socket.ConnectAsync($uri, [Threading.CancellationToken]::None).GetAwaiter().GetResult()

    $hello = Receive-ObsOp -Socket $socket -Op 0 -TimeoutSeconds $TimeoutSeconds
    $identify = @{
        rpcVersion = 1
        eventSubscriptions = 0
    }
    if ($hello.d.authentication) {
        if (-not $Password) {
            throw "OBS websocket requires a password. Pass -Password or set OBS_WEBSOCKET_PASSWORD."
        }
        $identify.authentication = ConvertTo-ObsAuth -Password $Password -Salt $hello.d.authentication.salt -Challenge $hello.d.authentication.challenge
    }

    Send-ObsJson -Socket $socket -Message @{ op = 1; d = $identify }
    Receive-ObsOp -Socket $socket -Op 2 -TimeoutSeconds $TimeoutSeconds | Out-Null
    Write-Host "Connected and identified."

    $sourceScene = "CoreVideo Sources"
    $slotScenes = @(1..$ParticipantCount | ForEach-Object { "CoreVideo Slot $_" })
    $participantSources = @(1..$ParticipantCount | ForEach-Object { "Zoom Participant $_" })
    $placeholderSources = @(1..$ParticipantCount | ForEach-Object { "CoreVideo Slot $_ Placeholder" })

    if (-not $AuditOnly) {
        Ensure-Scene -Socket $socket -Name $sourceScene
        Ensure-Scene -Socket $socket -Name $SceneName

        for ($i = 0; $i -lt $ParticipantCount; ++$i) {
            $participant = $participantSources[$i]
            $slotScene = $slotScenes[$i]
            $placeholder = $placeholderSources[$i]
            Ensure-Scene -Socket $socket -Name $slotScene
            Ensure-ColorInput -Socket $socket -SceneName $sourceScene -InputName $participant -Color 4279900698
            Ensure-ColorInput -Socket $socket -SceneName $slotScene -InputName $placeholder -Color 4281545523
            Ensure-SceneItem -Socket $socket -SceneName $sourceScene -SourceName $participant | Out-Null
            Ensure-SceneItem -Socket $socket -SceneName $slotScene -SourceName $participant | Out-Null
        }

        $columns = [Math]::Ceiling([Math]::Sqrt($ParticipantCount))
        $rows = [Math]::Ceiling($ParticipantCount / $columns)
        $tileW = 1920.0 / $columns
        $tileH = 1080.0 / $rows
        for ($i = 0; $i -lt $ParticipantCount; ++$i) {
            $slotScene = $slotScenes[$i]
            $itemId = Ensure-SceneItem -Socket $socket -SceneName $SceneName -SourceName $slotScene
            $x = ($i % $columns) * $tileW
            $y = [Math]::Floor($i / $columns) * $tileH
            Set-SceneItemBounds -Socket $socket -SceneName $SceneName -SceneItemId $itemId -X $x -Y $y -Width $tileW -Height $tileH -Index (20 + $i)
        }

        Invoke-ObsRequest -Socket $socket -RequestType "SetCurrentProgramScene" -RequestData @{ sceneName = $SceneName } -TimeoutSeconds $TimeoutSeconds | Out-Null
        Write-Host "Applied CoreVideo smoke scene '$SceneName'."
    }

    $sceneNames = Get-SceneNames -Socket $socket
    $inputNames = Get-InputNames -Socket $socket
    Assert-Contains -Actual $sceneNames -Expected @($sourceScene, $SceneName) -Label "Scenes"
    Assert-Contains -Actual $sceneNames -Expected $slotScenes -Label "Slot scenes"
    Assert-Contains -Actual $inputNames -Expected $participantSources -Label "Participant inputs"

    $sourceItems = @(Get-SceneItems -Socket $socket -SceneName $sourceScene | ForEach-Object { $_.sourceName })
    Assert-Contains -Actual $sourceItems -Expected $participantSources -Label "CoreVideo Sources scene items"

    for ($i = 0; $i -lt $ParticipantCount; ++$i) {
        $slotItems = @(Get-SceneItems -Socket $socket -SceneName $slotScenes[$i] | ForEach-Object { $_.sourceName })
        Assert-Contains -Actual $slotItems -Expected @($participantSources[$i]) -Label "$($slotScenes[$i]) scene items"
    }

    $lookItems = @(Get-SceneItems -Socket $socket -SceneName $SceneName | ForEach-Object { $_.sourceName })
    Assert-Contains -Actual $lookItems -Expected $slotScenes -Label "$SceneName scene items"

    Write-Host "OBS smoke test passed: $ParticipantCount participant source(s), $($slotScenes.Count) slot scene(s), and '$SceneName' are present and linked."
} finally {
    if ($socket.State -eq [System.Net.WebSockets.WebSocketState]::Open) {
        $socket.CloseAsync([System.Net.WebSockets.WebSocketCloseStatus]::NormalClosure, "CoreVideo smoke test complete", [Threading.CancellationToken]::None).GetAwaiter().GetResult() | Out-Null
    }
    $socket.Dispose()
}
