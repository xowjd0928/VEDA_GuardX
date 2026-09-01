# verify_encode.ps1 — 카메라 H.264 인코더 구조를 pcap 으로 재확증한다
#
# 무엇을 확인하나: **B 프레임이 없는지**(IPPPP).
#
# 왜 중요한가 — 이건 VMS 저지연 구조 전체의 전제다:
#   · 디코더를 `d3d11h264dec compliance=flexible` 로 쓸 수 있는 근거.
#     재정렬할 B 프레임이 없으니 DPB 제약을 풀어도 손해가 없다.
#   · 「패킷의 여정」 §6 가드레일 "B프레임 도입 금지 (IPPPP 유지)" 의 뿌리.
#   · ffmpeg 경로의 `reorder_queue_size=0` · `AV_CODEC_FLAG_LOW_DELAY` 도 같은 전제.
# 카메라 설정이 바뀌거나 장비를 교체하면 **이 전제부터 다시 확인해야 한다.**
#
# ⚠ 2026-08-10 에 이 스크립트를 만든 이유: 08-04 에 "IPPPP 확인" 이라는 **결론만**
#   기록돼 있었고 절차·원자료·분석 도구가 남아 있지 않았다(도구가 세션 스크래치패드
#   에 있었다). 가장 중요한 가정이 재현 불가 상태였다. 그래서 도구를 리포에 둔다.
#
# 사용법:
#   1) 캡처 (관리자 PowerShell, 카메라 IP 는 credentials.ini 의 [camera] host)
#      & "C:\Program Files\Wireshark\tshark.exe" -i <NIC> -f "host 192.168.0.3" `
#          -a duration:30 -w tools\results\camera.pcapng
#   2) 분석
#      powershell -ExecutionPolicy Bypass -File tools\verify_encode.ps1
#
# 판정: B(1·6) 가 0 이면 IPPPP. 하나라도 있으면 **가드레일이 깨진 것**이다.

param(
    [string]$Pcap   = "$PSScriptRoot\results\camera.pcapng",
    [string]$Tshark = "C:\Program Files\Wireshark\tshark.exe"
)

if (-not (Test-Path $Tshark)) { Write-Error "tshark 없음: $Tshark"; exit 2 }
if (-not (Test-Path $Pcap))   { Write-Error "캡처 없음: $Pcap";     exit 2 }

# ⚠ RTP payload type 을 h264 로 **명시 매핑**해야 한다.
#   이 카메라는 동적 PT 를 쓴다 — 98=H264, 107=vnd.onvif.metadata (SDP 확인).
#   매핑 없이 `-Y h264` 만 주면 결과가 0줄로 나와 "B 프레임 없음"으로 오독한다.
#   PT 는 장비·프로파일마다 다를 수 있으므로 먼저 확인하는 편이 안전하다:
#     tshark -r <pcap> -Y rtp -T fields -e rtp.p_type | sort | uniq -c
$Pt = 98

Write-Host "=== 캡처: $(Split-Path $Pcap -Leaf) ($([math]::Round((Get-Item $Pcap).Length/1MB,1)) MB) ==="

# slice_type: 0·5=P · 1·6=B · 2·7=I  (H.264 7.4.3)
$raw = & $Tshark -r $Pcap -d "rtp.pt==$Pt,h264" -Y "h264" -T fields -e h264.slice_type 2>$null
$types = $raw -split "[,`r`n]" | Where-Object { $_ -match '^\d+$' }

if ($types.Count -eq 0) {
    Write-Error @"
슬라이스를 한 개도 못 읽었다 — 판정 불가.
  · RTP payload type 이 $Pt 이 아닐 수 있다 (위 주석의 확인 명령 참고)
  · 캡처에 영상 RTP 가 없을 수 있다 (RTSP 협상만 잡힌 경우)
'B 프레임 없음' 으로 오독하지 말 것.
"@
    exit 2
}

$p = ($types | Where-Object { $_ -eq '0' -or $_ -eq '5' }).Count
$b = ($types | Where-Object { $_ -eq '1' -or $_ -eq '6' }).Count
$i = ($types | Where-Object { $_ -eq '2' -or $_ -eq '7' }).Count

"  표본 슬라이스 : $($types.Count)"
"  P (0·5)       : $p"
"  B (1·6)       : $b"
"  I (2·7)       : $i"
if ($i -gt 0) { "  I:P 비율      : 1 : $([math]::Round($p/$i,1))   (GOV 설정과 대조할 값)" }
""

if ($b -eq 0) {
    Write-Host "PASS — B 슬라이스 0개. IPPPP 구조 확인." -ForegroundColor Green
    exit 0
} else {
    Write-Host "FAIL — B 슬라이스 $b 개 검출. 「패킷의 여정」 §6 가드레일이 깨졌다." -ForegroundColor Red
    Write-Host "       flex 디코더 안전성·저지연 전제가 무효다. 카메라 인코더 설정을 확인할 것." -ForegroundColor Red
    exit 1
}
