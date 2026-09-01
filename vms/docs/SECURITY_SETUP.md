# GuardX VMS — 보안 설정

> **누가 읽나:** VMS를 자기 PC에서 빌드·실행하는 모든 팀원.
> **언제 읽나:** ① 새 PC에서 처음 돌릴 때 ② **카메라를 교체·초기화한 뒤**
> ③ `SSL handshake failed` / `인증서 지문 불일치` 가 뜰 때.
>
> 최종 갱신: 2026-08-03

---

## 0. 30초 요약

카메라는 HTTPS로만 응답한다(HTTP는 301로 리다이렉트). 인증서는 **Hanwha 사설
루트**가 발급해 윈도우 신뢰 저장소에 없고, 우리는 IP로 접속하니 CN
(`*.hanwha-security.com`)도 안 맞는다. 게다가 이 카메라는 체인에 **리프 1장만**
보낸다(중간·루트 없음 — 2026-08-03 확인). 그래서 일반적인 인증서 검증은 원리적으로
통과할 수 없다.

대신 **인증서를 리포에 동봉**한다. VMS는 시작할 때 `:/certs/*.pem` 을 전부 읽어
SHA-256 지문 목록을 만들고, 접속한 상대의 인증서가 그중 하나와 **완전히 일치**할
때만 받아들인다.

> **➜ 팀원이 할 일: pull 하고 빌드. 끝.**
> 지문을 손으로 옮겨 적는 절차는 없어졌다 (2026-08-03 이전 방식).

신뢰 강도는 지문 고정과 같다(인증서 완전 일치). 사설 루트 CA를 통째로 신뢰하는
방식이 **아니므로**, 같은 CA가 발급한 다른 한화 장비까지 덩달아 신뢰하지 않는다.

> [!note] 2026-08-10 — 핀 대조가 **두 지점**에서 돈다 (구멍 하나를 막았다)
> 그전에는 핀 대조가 `sslErrors` 핸들러 **안에서만** 돌았다. 그러면 체인 검증을
> **통과한** 인증서는 핀을 아예 안 본다 — 윈도우 신뢰 저장소에 있는 CA가 발급한
> 인증서면 그대로 통과다. 사설 CA가 어느 PC의 저장소에 들어가 있거나, 공인 CA
> 인증서를 내미는 중간자가 있으면 뚫린다.
>
> 지금 이 현장의 카메라는 사설 인증서라 **항상** `sslErrors` 를 거치므로 실제로
> 뚫리고 있지는 않았다. 고친 이유는 그 사실이 **카메라와 PC 설정에 달려 있기**
> 때문이다 — 방어가 우연히 성립하고 있는 상태였다.
>
> 수정: 판정을 `peer_pin_matches()` 하나로 빼고
> `QNetworkAccessManager::encrypted`(핸드셰이크 성공 경로)와 `sslErrors`(검증 실패
> 경로) **둘 다**에서 부른다. `encrypted` 에서 불일치면 `reply->abort()` 로
> **요청 본문(Digest 자격 포함)이 나가기 전에** 끊는다. 판정을 복제하지 않았으므로
> 두 자리가 어긋날 수 없다.

---

## 1. 새 PC에서 처음 돌릴 때

### ① 자격 파일

위치 — **리포 안이 아니다** (소스와 함께 새어나가지 않게 일부러 밖에 둔다):

```
%LOCALAPPDATA%\GuardX\credentials.ini
```

본인 계정만 넣으면 된다. **인증서 관련 항목은 필요 없다.**
평문으로 적어도 되고 (`read_secret()` 은 `dpapi:` 접두어가 없으면 그대로 읽는다):

```ini
[camera]
host=192.168.0.3
user=<카메라 계정>
password=<비밀번호>
```

**`[mqtt]` 섹션은 이제 없어도 된다** (08-12). 기본값이
`172.20.33.251:8883`(mTLS) 이라 **인증서만 두면 붙는다** — 08-12 에 기본값이
1883 이던 탓에, 인증서를 받은 팀원 전원이 평문 리스너에 TLS 를 던지며
"연결 안 됨"만 보던 사고가 있었다. 그 재발 방지다.
(서버 인증서 SAN 에 `DNS:rpib` 와 `IP:172.20.33.251` 이 둘 다 있어
IP 접속도 hostname 검증을 통과한다 — 08-12 실측.)

**인증서 경로는 ini 에 안 적는다.** 정해진 자리에 정해진 이름으로 두면 된다:

```
C:\Users\<계정>\AppData\Local\GuardX\certs\
    ca.crt        ← CA 공개 인증서
    <cn>.crt      ← 예: vms-3-11.crt  (파일명 = CN = 계정)
    <cn>.key      ← ⚠ 개인키
```

`<cn>` 은 `vms-<호스트이름>` 이다(로그 첫 줄에 찍힌다). `cmd` 에서 `hostname`
을 치면 나온다.

| 상태 | 동작 |
|---|---|
| 세 파일이 다 있다 | **mTLS 로 붙는다** (로그에 찾은 폴더를 함께 찍는다) |
| 하나라도 없다 | ⚠ **접속을 포기한다.** 평문으로 내려가지 **않는다** — 없는 파일 이름과 둬야 할 자리를 **로그와 로그인 화면 둘 다에** 말한다 |
| `[mqtt] tls=0` | 평문(1883 롤백용). ⚠ **`port=1883` 도 함께 바꿔야 한다** — 8883 은 TLS 리스너라 평문이면 브로커가 끊는다(로그·화면이 알려준다) |

다른 자리에 둬야 하면 `[mqtt] tls_ca/tls_cert/tls_key` 로 덮을 수 있다.

**로그인 화면 하단 상태줄이 이유를 말한다** (08-12). "연결 안 됨 — 서버 확인 중"
(재시도 중, 노랑)과 **설정 오류(빨강 — 기다려도 안 붙는다)** 를 구분한다:

| 화면 문구 | 뜻 | 할 일 |
|---|---|---|
| `certificate missing: <파일>` | 인증서가 없거나 자리가 틀렸다 | 위 폴더에 3종 배치 |
| `tls=0 but port is 8883` | 롤백을 절반만 했다 | `port=1883` 도 적거나 `tls=0` 제거 |
| `broker refused the connection` | 브로커가 CONNACK 에서 거절 | 인증서 CN·브로커 ACL 확인 |
| `연결 안 됨 — 서버 확인 중` | 설정은 정상, 서버가 안 보인다 | 서버·망 확인 (재시도가 알아서 돈다) |

> [!danger] 인증서가 없으면 **안 붙는다** — 이건 의도된 동작이다
> "인증서가 없으니 평문으로라도 붙자"는 자동 강등은 보안 설정이 조용히 사라지는
> 가장 흔한 방식이다. 게다가 8883 은 어차피 브로커가 끊어 rc=7 재시도만 돈다 —
> 그 상태는 원인이 안 보인다. 그래서 **붙지 않고 이유를 말한다.**

> **브로커 mTLS (8883) — 2026-08-11 · 08-12 갱신**
>
> ⚠ **`host` 는 인증서 SAN 에 있는 값이어야 한다** — `rpib`(사내 DNS/Tailscale)
> 또는 `172.20.33.251`(LAN IP, 기본값). Tailscale IP(`100.73.217.52`)나
> FQDN(`rpib.tail…ts.net`)은 SAN 에 없어서 hostname 검증에서 끊긴다. 우리는
> `mosquitto_tls_insecure_set()` 을 **쓰지 않는다**(그걸 켜면 "상대가 그 서버가
> 맞는가"라는 mTLS 의 절반이 사라진다).
>
> **계정은 인증서 CN 이 정한다** — 브로커가 `use_identity_as_username true` 라
> CN 이 그대로 계정·ACL 기준이다. `[mqtt] client_id` 로 **세션 id** 를 바꿔도
> 계정은 안 바뀐다(08-12 분리 — 한 PC 에서 앱 두 개를 병행할 때
> 한쪽만 `client_id=vms-3-11-b` 처럼 주면 서로 세션을 안 끊는다.
> 두 인스턴스가 같은 ini 를 읽으므로 둘째 쪽은 `GUARDX_CREDENTIALS` 환경변수로
> 별도 ini 를 준다).
> `MqttLink` 가 인증서 파일명과 client_id 가 다르면 경고를 남긴다.
> ⚠ **`.key` 는 리포·공유폴더·OneDrive 에 두지 않는다.** 자격 파일 옆
> (`%LOCALAPPDATA%\GuardX\certs\`)이 가장 안전하다 — 그 폴더는 이미 OneDrive
> 밖이고 credentials.ini 와 수명이 같다. 인증서는 기기마다 다르다(`vms-3-11` 등).
> ⚠ **경로를 사람이 적지 않는 이유**: 08-11 에 인증서를 옮긴 뒤 ini 를 안 고쳐
> 브로커에 못 붙고 화면이 비었다. 경로가 코드 한 곳에서 정해지면 그 사고가
> 생길 자리가 없다. (그때도 **평문으로 내려가지는 않았다** — 접속을 포기하고
> `TLS 파일이 없다: …` 로 원인을 이름까지 찍었다.)
> ⚠ 파일이 없거나 `tls_set` 이 실패하면 **평문으로 내려가지 않고 접속을 포기**한다.
> 자동 강등은 보안 설정이 조용히 사라지는 가장 흔한 방식이다.

> `[database]` 섹션은 **v6부터 읽지 않는다** (`credentials.cpp` 주석 참조).
> VMS는 DB에 직접 붙지 않고 MQTT `guardx/db/rpib/*` 로만 받는다.
> 남아 있으면 쓰이지도 않는 비밀번호가 디스크에 남는 것뿐이니 **지우는 게 낫다.**

### ② (선택) 비밀값 암호화

```powershell
gstream_VMS.exe --encrypt-credentials
```

`user`/`password` 를 DPAPI로 묶어 `dpapi:...` 형태로 바꾼다.
비밀번호를 디스크에 아예 안 두려면 환경변수를 쓴다 — 파일보다 우선한다:

```
GUARDX_CAM_USER · GUARDX_CAM_PASSWORD · GUARDX_MQTT_USER · GUARDX_MQTT_PASSWORD
```

### ③ 확인 (팀원 온보딩 체크)

**인증서는 리포에 들어 있으므로 따로 할 일이 없다.** pull + 빌드로 끝이고,
지문을 손으로 옮겨 적는 절차는 2026-08-03 부로 없어졌다. 아래는 그게 실제로
먹었는지 확인하는 방법이다.

```powershell
.\gstream_VMS.exe 2> run.log
```

```powershell
Select-String run.log -Pattern "SSL handshake|핀 미설정|지문 불일치|신뢰 인증서 없음"
```

**아무것도 안 나오면 정상이다.** 추가로 `[CameraTuner]`·`[CrowdPage]` 가
데이터를 받으면 전 경로가 살아 있는 것이다.

동봉 인증서가 실행 파일에 실제로 들어갔는지는 이걸로도 볼 수 있다:

```powershell
Select-String -Path .\gstream_VMS.exe -Pattern "BEGIN CERTIFICATE" -Encoding Byte -ErrorAction SilentlyContinue | Measure-Object | Select-Object Count
```

`Count` 가 0이면 `certs/*.pem` 없이 빌드된 것이다 — CMake 가 경고를 냈을 테니
빌드 로그를 확인하고 다시 빌드할 것.

| 증상 | 원인 | 조치 |
|---|---|---|
| 오류 없음 + 데이터 수신 | ✅ | 끝 |
| `자격 파일이 없습니다` | `credentials.ini` 미생성 | §1-① |
| `[camera] user/password가 비어 있습니다` | 남의 파일을 복사했다(DPAPI) | 본인 것으로 새로 (§3) |
| `신뢰 인증서 없음` | `certs/` 없이 빌드됨 | 위 Count 확인 후 재빌드 |
| `지문 불일치` | 카메라가 교체·초기화됐다 | §2 로 갱신·커밋 |
| **타임아웃** | 인증서 아님 — 경로 문제 | 현장 LAN 이라 Tailscale `accept-routes` 필요 |
| 검은 타일 4개 + `무프레임` | 인증서 아님 — 프로파일명 불일치 | 카메라의 실제 프로파일 확인 |

---

## 2. 카메라를 교체·초기화했을 때 (인증서 갱신)

인증서가 바뀌면 **한 사람이 한 번** 갱신해 커밋하면 전원에게 퍼진다.

```bash
openssl s_client -connect 192.168.0.3:443 -showcerts </dev/null 2>/dev/null \
  | openssl x509 -out certs/camera-hanwha.pem
```

```bash
openssl x509 -in certs/camera-hanwha.pem -noout -subject -issuer -enddate -fingerprint -sha256
```

`subject`/`issuer` 가 한화 것이 맞는지 눈으로 확인하고 커밋한다.
PEM 머리말의 지문·추출일 주석도 같이 고칠 것 (파일만 봐도 이력이 남게).

> **무중단 교체:** 새 PEM 을 `certs/` 에 **추가**하고 (옛 것을 지우지 말고)
> 교체가 끝난 뒤 옛 것을 지운다. 그 사이엔 둘 다 허용되므로 끊기지 않는다.
> `certs/` 안의 모든 PEM 이, 한 파일에 여러 장이 들어 있어도 전부 허용된다.

## 2-1. 예외: 다른 카메라를 붙일 때

리포를 고치지 않고 **자기 PC만** 예외 처리하고 싶으면 TOFU 경로가 남아 있다:

```powershell
gstream_VMS.exe --pin-camera-cert
```

지금 응답하는 상대의 인증서를 그대로 믿어 `credentials.ini` 에
`cert_sha256` 으로 적는다. 이 값은 동봉 인증서에 **더해서** 허용된다.
GUI 앱이라 콘솔에 아무것도 안 찍히니 파일로 확인한다:

```powershell
Select-String "$env:LOCALAPPDATA\GuardX\credentials.ini" -Pattern "cert_sha256|^https"
```

> ⚠ TOFU는 "지금 붙은 상대"를 검증 없이 믿는 것이다. **공용 카메라에는 쓰지 말 것**
> — 그건 §2 의 커밋 경로로 처리한다.

---

## 3. 절대 하지 말 것

| 하지 말 것 | 왜 |
|---|---|
| **`credentials.ini` 를 남에게 주기** | 비밀값이 DPAPI **사용자 범위**(`CryptProtectData`)로 묶여 다른 계정에선 복호화가 안 된다 → `DPAPI 복호화 실패` 후 빈 값으로 기동 실패. 게다가 비밀번호가 그대로 넘어간다 |
| **`credentials.ini` 를 리포에 커밋** | 기본 위치가 리포 밖인 이유가 이것. `.gitignore` 에 방어선을 뒀지만 위치를 바꾸지 말 것 |
| **`certs/` 를 지우고 빌드** | 허용 지문이 비어 카메라 HTTPS 요청이 전부 거부된다. CMake 가 경고를 낸다 |
| **`ignoreSslErrors` 를 무조건 호출하도록 고치기** | 검증 자체가 무의미해진다. 지문 대조 **후에만** 부른다 |
| **카메라 HTTPS를 꺼서 회피** | 200ms 폴링마다 카메라 비밀번호가 평문으로 망을 지나간다 |

**인증서(`certs/*.pem`)는 비밀이 아니다.** 카메라가 접속자 누구에게나 건네주는
공개 정보라 리포에 있어도 새는 것이 없다. 위 금지 항목은 **`credentials.ini`**
(계정·비밀번호) 이야기다. 둘을 헷갈리지 말 것.

---

## 4. RPi B 쪽은 **별개다**

폴러는 VMS와 **다른 방식**으로 고정한다 — 인증서 전체가 아니라 **공개키** 핀
(`sha256//base64`, curl `CURLOPT_PINNEDPUBLICKEY`). 두 값은 형식이 달라 서로
복사할 수 없다. 카메라를 초기화했으면 **양쪽 다** 해야 한다.

```bash
sudo bash setup_security.sh --refresh-cert
```

> **`--refresh-cert` 를 빠뜨리지 말 것.** 없으면 스크립트가 기존 `camera.pem` 을
> 보고 "이미 존재 — 유지" 하며 1단계를 건너뛴다. 초기화 **전** 인증서가 그대로
> 고정된 채 남아 조용히 실패한다.

스크립트는 인증서 추출·핀 계산 외에 파일 권한 600, systemd 샌드박스 드롭인,
폴러 재시작·검증까지 한다. 자세한 건 `rpi_b/setup_security.sh` 주석 참조.

---

## 5. 증상별 진단

| 로그 | 원인 | 조치 |
|---|---|---|
| `[Credentials] 인증서 지문 불일치 — 거부` | 카메라 인증서가 바뀌었다(교체·초기화) **또는 중간자** | §2 로 갱신·커밋 |
| `[Credentials] 신뢰 인증서 없음 — TLS 연결 거부` | `certs/` 가 리소스에 안 들어갔다 | `certs/*.pem` 존재 확인 후 재빌드 |
| `[Credentials] 동봉 인증서를 못 읽음` | PEM 이 깨졌다 | `openssl x509 -in <파일> -noout -subject` 로 확인 |
| `[Credentials] 핀은 일치하나 예상 밖 오류` | 만료·폐기 등 지문이 보장 못 하는 문제 | 종류별 **1회만** 찍힌다. 내용 보고 판단 |
| 같은 경고가 **초당 수십 줄** | `credentials.cpp` 가 2026-08-03 이전 버전 | pull 후 재빌드 |
| `[Credentials] DPAPI 복호화 실패` | 남의 `credentials.ini` 를 가져왔다 | 본인 것으로 새로 만든다 (§1-①) |
| **타임아웃** (인증서 오류 아님) | 경로 문제 — 카메라가 현장 LAN에 있다 | Tailscale `accept-routes` 켜고 접속, 또는 현장에서 |
| 검은 타일 4개 + `무프레임` 재접속 | 인증서와 무관 — **프로파일명 불일치** | `HKCU\Software\GuardX\VMS\grid_profile`·`fullscreen_profile` 을 카메라의 실제 프로파일로 |

---

## 6. 카메라 공장초기화 후 체크리스트

초기화는 인증서만 날리는 게 아니다. 순서가 있다 —
상세는 Obsidian `카메라 초기화 복구 체크리스트`.

- [ ] IP·비밀번호 복구
- [ ] **NTP 먼저** (시계가 틀린 채 앱을 켜면 학습 데이터가 오염된다)
- [ ] 채널별 AI 켜기 + 라인 재작도
- [ ] 프로파일 확인 — **VMS 레지스트리의 프로파일명과 일치해야 한다**
- [ ] `.cap` 재설치 + Auto Start
- [ ] **인증서 갱신 + 커밋** (§2) ← 이거 하나로 팀 전원 해결
- [ ] **RPi B**: `setup_security.sh --refresh-cert` (§4)

---

## 7. 남은 보안 항목 (제출·발표 전)

이 문서 범위 밖이지만 같이 봐야 하는 것들 — 절차는 Obsidian
`RPi B 실기기 전용 TODO`:

- SSH 키 전용 전환 (수동 — 원격 락아웃 위험, 폴백 세션 열어두고)
- 브로커 mTLS 2단계 배치
- 카메라 폴링 전용 계정 전환 (현재 `admin`)
- DB 비밀번호 회전 (VMS 접속 문자열 동시 갱신)
- git 이력·docs 평문 자격증명 정리 (공개 직전)

---

## 관련 코드

| 무엇 | 위치 |
|---|---|
| 동봉 인증서 → 허용 지문 목록 | `credentials.cpp` `bundled_pins()` |
| **핀 대조 판정 (단일 원천)** | `credentials.cpp` `peer_pin_matches()` |
| 핀 검사 배선(`encrypted` + `sslErrors`)·예상 오류 목록 | `credentials.cpp` `install_tls_pinning()` |
| 핀 일치 자동 확인 | `tools/acceptance.py devices` (동봉 PEM 지문 vs 실제 피어) |
| 지문 조회·기록 (TOFU, 예외 경로) | `credentials.cpp` `pin_current_camera_cert()` |
| DPAPI 암·복호화 | `credentials.cpp` `protect()` / `unprotect()` |
| 인증서 리소스 등록 | `CMakeLists.txt` `vms_certs` |
| CLI 플래그 | `main.cpp` (`--pin-camera-cert`, `--encrypt-credentials`) |
| 폴러 프로비저닝 | `rpi_b/setup_security.sh` |
