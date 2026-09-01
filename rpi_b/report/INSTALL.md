# GuardX 예측 리포트 — 설치 (RPi B, 1회)

브라우저로 열면 항상 최신인 자동 리포트. 구성: `gen_report.sh`(root cron,
10분마다 DB·카메라에서 데이터 생성) + `index.html`(정적, fetch로 소비) +
`python3 -m http.server`(systemd, :8088).

## 배포 (VM에서)

```bash
rsync -av /media/sf_fp_shared_folder/rpi_b/report/ juan@172.20.33.251:~/guardx-report/
```

## 설치 (Pi에서, 1회)

```bash
ssh juan@172.20.33.251

# ① 디렉터리 + 파일 배치
sudo mkdir -p /srv/guardx-report
sudo cp ~/guardx-report/index.html /srv/guardx-report/
sudo cp ~/guardx-report/gen_report.sh /usr/local/bin/guardx-gen-report.sh
sudo chmod 700 /usr/local/bin/guardx-gen-report.sh   # 카메라 자격증명 포함

# ② 첫 실행으로 검증 (data/ 파일 4개 + generated_at 생성돼야)
sudo /usr/local/bin/guardx-gen-report.sh
ls -la /srv/guardx-report/data/

# ③ cron (root — sudo -u postgres가 무암호로 되는 계정)
sudo crontab -l 2>/dev/null | { cat; echo "*/10 * * * * /usr/local/bin/guardx-gen-report.sh"; } | sudo crontab -

# ④ 웹서버 (systemd)
sudo tee /etc/systemd/system/guardx-report.service > /dev/null <<'EOF'
[Unit]
Description=GuardX report static server
After=network-online.target
[Service]
User=juan
ExecStart=/usr/bin/python3 -m http.server 8088 --directory /srv/guardx-report
Restart=always
[Install]
WantedBy=multi-user.target
EOF
sudo systemctl enable --now guardx-report

# ⑤ 확인
curl -s http://localhost:8088/data/generated_at.txt
```

## 사용

- 사무실: **http://172.20.33.251:8088**
- 어디서든 (tailscale): **http://100.73.217.52:8088**
- 페이지는 5분마다 자동 새로고침, 데이터는 10분마다 갱신.

## 내용물

| 카드 | 데이터 출처 |
|---|---|
| 타일 (프로파일 일수·MAE·지금 인원·5분 예측) | 카메라 /prediction |
| 하루 프로파일 vs 오늘 실측 | 카메라 /forecast_day + DB detections(15분 빈) |
| 예측 vs 실측 최근 24h | DB congestion_prediction ⋈ detections (분 중앙값, 0패딩) |
| 실전 MAE 표 (7일 창) | 〃 — 클라이언트 계산 |

## 운영 메모

- 쿼리는 `sudo -u postgres`(peer) — guardx_admin GRANT 버그와 무관하게 동작.
  GRANT 정리되면 gen 스크립트를 일반 계정+PGCONN으로 바꿔도 됨.
- 카메라 접근은 Pi의 tailscale 경유(192.168.0.3) — 서브넷 라우터가 죽으면
  프로파일 카드만 낡고(카메라 fetch 실패 시 기존 파일 유지) DB 카드는 계속 갱신.
- 부분 파일 서빙 방지: gen이 .tmp에 쓰고 원자적 mv.
- 제거: `sudo systemctl disable --now guardx-report` + crontab 항목 삭제.
