<#
.SYNOPSIS
  혼잡 경보(AlertFeed / AlertPopup / LIVE 타일) 시나리오 테스트

.DESCRIPTION
  폴러를 기다리지 않고 경보 UI를 직접 검증한다. 폴러는 60초 틱에서만
  판정하므로(pred_interval_s) 실환경 확인은 케이스당 최대 1분이 걸린다 —
  이 스크립트는 그 대기를 없앤다.

  ⚠ 기본은 **격리 브로커(127.0.0.1:11883)** 다. 실브로커로 쏘면
  transmission layer의 실제 액추에이터가 반응할 수 있다. 실브로커로
  시험하려면 담당자 동의를 받고, 끝나면 반드시 -Case clear 로 원복할 것.

  ⚠ payload는 반드시 파일로 넘긴다 (mosquitto_pub -f). PowerShell이
  네이티브 인자의 큰따옴표를 먹어서 -m '{"a":1}' 은 {a:1} 로 도착한다.

.EXAMPLE
  # 1) 격리 브로커 띄우기
  & "C:\Program Files\mosquitto\mosquitto.exe" -p 11883

  # 2) VMS를 격리 브로커로 물려서 실행 (test_credentials.ini 는 [mqtt] 127.0.0.1:11883)
  $env:GUARDX_CREDENTIALS="...\test_credentials.ini"; .\gstream_VMS.exe

  # 3) 시나리오 실행
  .\alert_test.ps1 -Case all -Shot
#>
param(
  [string]$Broker = "127.0.0.1",
  [int]$Port = 11883,
  [ValidateSet('critical','warn','clear','multi','snapshot','snapshot-empty',
               'stale','bad','all')]
  [string]$Case = 'all',
  [switch]$Shot,                       # 케이스마다 창 캡처
  [string]$OutDir = "$env:TEMP\alert_test",
  [int]$SettleMs = 2500                # 발행 후 UI 반영 대기
)

$ErrorActionPreference = 'Stop'
$pub = "C:\Program Files\mosquitto\mosquitto_pub.exe"
if (-not (Test-Path $pub)) { throw "mosquitto_pub 없음: $pub" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$script:step = 0

function Send-Payload {
  param([string]$Topic, [string]$Json, [switch]$Retain)
  $f = Join-Path $OutDir "payload.json"
  # BOM이 앞에 붙으면 QJsonDocument 파싱이 깨진다
  [System.IO.File]::WriteAllText($f, $Json, $Utf8NoBom)
  $args = @('-h', $Broker, '-p', $Port, '-t', $Topic, '-f', $f, '-q', '1')
  if ($Retain) { $args += '-r' }
  & $pub @args
  if ($LASTEXITCODE -ne 0) { throw "publish 실패 ($Topic)" }
}

function Alert-Json {
  param([int]$Channel, [string]$Severity, [int]$Count, [int]$Capacity,
        [string]$Source = 'prediction', [int]$IncidentId = 9000,
        [int64]$Ts = 0)
  if ($Ts -eq 0) { $Ts = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() }
  $zone = $Channel + 1
  '{"node_id":"rpib","timestamp":' + $Ts + ',"seq":1,"event":"congestion"' +
  ',"zone_id":' + $zone + ',"channel":' + $Channel +
  ',"incident_id":' + $IncidentId + ',"severity":"' + $Severity + '"' +
  ',"count":' + $Count + ',"capacity":' + $Capacity +
  ',"source":"' + $Source + '"}'
}

function Snapshot-Json {
  param([array]$Incidents, [int64]$Ts = 0)
  if ($Ts -eq 0) { $Ts = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() }
  $items = ($Incidents | ForEach-Object {
    $cnt = if ($null -ne $_.count) { $_.count } else { -1 }
    $cap = if ($null -ne $_.cap) { $_.cap } else { -1 }
    '{"incident_id":' + $_.id + ',"zone_id":' + ($_.ch + 1) +
    ',"channel":' + $_.ch + ',"severity":"' + $_.sev + '"' +
    ',"source_type":"' + $_.src + '"' +
    ',"count":' + $cnt + ',"capacity":' + $cap +
    ',"detected_at":"2026-07-30T06:00:00Z"}'
  }) -join ','
  '{"node_id":"rpib","timestamp":' + $Ts + ',"incidents":[' + $items + ']}'
}

function Run-Step {
  param([string]$Name, [string]$Expect, [scriptblock]$Body)
  $script:step++
  $tag = '{0:d2}_{1}' -f $script:step, $Name
  Write-Host ""
  Write-Host "[$tag]" -ForegroundColor Cyan
  Write-Host "  기대: $Expect"
  & $Body
  Start-Sleep -Milliseconds $SettleMs
  if ($Shot) {
    $shot = Join-Path $PSScriptRoot 'shot.ps1'
    if (Test-Path $shot) {
      & powershell -ExecutionPolicy Bypass -File $shot -Out (Join-Path $OutDir "$tag.png") | Out-Null
      Write-Host "  캡처: $OutDir\$tag.png"
    }
  }
}

$ALERT = 'guardx/alert/rpib'
$SNAP  = 'guardx/db/rpib/incidents'

Write-Host "브로커 $Broker`:$Port · 케이스 $Case" -ForegroundColor Yellow
if ($Broker -ne '127.0.0.1' -and $Broker -ne 'localhost') {
  Write-Warning "실브로커 대상이다 — transmission layer가 반응할 수 있다. 끝나면 -Case clear 로 원복할 것."
}

$all = ($Case -eq 'all')

# ── 1. 평상 → critical (라이브 전이) ────────────────────────────────────
if ($all -or $Case -eq 'critical') {
  Run-Step 'critical' 'CH1 빨간 3px 테두리 점멸 + "CRITICAL 55/60 예측" 칩 + 팝업 + LIVE 배지 1' {
    Send-Payload -Topic $ALERT -Json (Alert-Json -Channel 0 -Severity 'critical' -Count 55 -Capacity 60)
  }
}

# ── 2. 다른 채널 warn (색만, 팝업 없음) ─────────────────────────────────
if ($all -or $Case -eq 'warn') {
  Run-Step 'warn' 'CH3 앰버 2px 테두리(점멸 없음) + "WARN 33/40" 칩. 팝업은 뜨지 않는다' {
    Send-Payload -Topic $ALERT -Json (Alert-Json -Channel 2 -Severity 'warn' -Count 33 -Capacity 40 -Source 'detection' -IncidentId 9002)
  }
}

# ── 3. 다중 critical ───────────────────────────────────────────────────
if ($all -or $Case -eq 'multi') {
  Run-Step 'multi' '팝업 제목에 CH1, CH2 둘 다 · LIVE 배지 2' {
    Send-Payload -Topic $ALERT -Json (Alert-Json -Channel 1 -Severity 'critical' -Count 78 -Capacity 80 -IncidentId 9003)
  }
}

# ── 4. 잘못된 입력 (무시되어야 함, 크래시 금지) ─────────────────────────
if ($all -or $Case -eq 'bad') {
  Run-Step 'bad' '아무 변화 없음. 앱이 살아 있어야 한다 (파싱 실패·범위 밖 채널 무시)' {
    Send-Payload -Topic $ALERT -Json '{"node_id":"rpib","event":"congestion","channel":99,"severity":"critical"}'
    Send-Payload -Topic $ALERT -Json '{ this is not json'
    Send-Payload -Topic $ALERT -Json '{"event":"fire","channel":0,"severity":"critical"}'
  }
}

# ── 5. 해제 ────────────────────────────────────────────────────────────
if ($all -or $Case -eq 'clear') {
  Run-Step 'clear' '해당 채널 테두리·칩 원복. 전부 해제되면 팝업 자동 닫힘' {
    Send-Payload -Topic $ALERT -Json (Alert-Json -Channel 0 -Severity 'clear' -Count 2 -Capacity 60)
    Send-Payload -Topic $ALERT -Json (Alert-Json -Channel 1 -Severity 'clear' -Count 3 -Capacity 80 -IncidentId 9003)
    Send-Payload -Topic $ALERT -Json (Alert-Json -Channel 2 -Severity 'clear' -Count 1 -Capacity 40 -Source 'detection' -IncidentId 9002)
  }
}

# ── 6. retained 스냅샷 복원 (앱을 나중에 켠 상황) ───────────────────────
#    이게 실전에서 제일 중요하다: 경보는 전이 때만 쏘므로, 이미 열린
#    incident는 이 토픽이 없으면 VMS가 영영 모른다.
if ($all -or $Case -eq 'snapshot') {
  Run-Step 'snapshot' 'CH1 critical 복원 + 칩에 "2/1" 숫자 (앱 재시작 후에도 유지)' {
    Send-Payload -Topic $SNAP -Retain -Json (Snapshot-Json -Incidents @(
      @{ id = 7; ch = 0; sev = 'critical'; src = 'detection'; count = 2; cap = 1 }))
  }
}

# ── 7. 낡은 스냅샷은 최신 라이브를 덮지 않아야 한다 ─────────────────────
#    폴러 스냅샷은 30초 틱이라 라이브보다 늦게 도착할 수 있다. 그대로
#    적용하면 방금 해제된 채널이 30초 동안 되살아난다.
if ($all -or $Case -eq 'stale') {
  Run-Step 'stale' 'CH1이 해제 상태를 유지해야 한다 (낡은 critical 스냅샷 무시)' {
    $now = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
    Send-Payload -Topic $ALERT -Json (Alert-Json -Channel 0 -Severity 'clear' -Count 1 -Capacity 60 -Ts $now)
    Start-Sleep -Milliseconds 400
    # 10초 전 시각의 critical 스냅샷 — 무시되어야 한다
    Send-Payload -Topic $SNAP -Retain -Json (Snapshot-Json -Ts ($now - 10000) -Incidents @(
      @{ id = 7; ch = 0; sev = 'critical'; src = 'detection' }))
  }
}

# ── 8. 스냅샷으로 전체 해제 ────────────────────────────────────────────
if ($all -or $Case -eq 'snapshot-empty') {
  Run-Step 'snapshot-empty' '모든 채널 평상 복귀 (retained 비우기 — 다음 테스트 오염 방지)' {
    Send-Payload -Topic $SNAP -Retain -Json (Snapshot-Json -Incidents @())
  }
}

Write-Host ""
Write-Host "완료. 캡처: $OutDir" -ForegroundColor Green
