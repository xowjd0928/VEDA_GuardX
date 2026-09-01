# VMS 개발 PC 세팅 매뉴얼 (Windows)

> 대상: VMS를 처음 빌드·실행하는 사람
> 검증 환경: Windows 11 · Qt 6.11.1 MSVC2022 64bit · VS 2022(18) Community
>            mosquitto 2.1.2 · GStreamer 1.x MSVC x86_64
> 관련: `VMS_CODE_MAP.md`(코드 구조) · `DB_LINK_AND_MQTT_MIGRATION.md`(설계)
> 작성: 2026-07-30

---

## 0. 먼저 — 왜 클론만으로는 안 되는가

리포를 받아도 **세 가지가 없어서** 안 돌아간다.

| # | 없는 것 | 왜 | 증상 |
|---|---|---|---|
| 1 | **libmosquitto** | 시스템 라이브러리라 리포에 넣을 수 없다 | 앱은 뜨는데 정원·히트맵이 비어 있음 |
| 2 | **`credentials.ini`** | 비밀번호가 들어가 `.gitignore` | 앱이 아예 안 뜸 (설정 필요 메시지박스) |
| 3 | GStreamer (권장) | 시스템 설치 | 영상이 안 나오거나 지연 1초+ |

**1번이 가장 헷갈린다.** 없어도 **빌드가 성공하고 앱도 뜬다** — CMake가 조용히
무동작 스텁으로 바꿔 넣기 때문이다. 그래서 "왜 히트맵만 안 나오지" 하고
한참 헤매게 된다.

---

## 1. 필수 설치

### 1-1. Visual Studio 2022 (MSVC 툴체인)

Community 판이면 충분. 설치 시 **"C++를 사용한 데스크톱 개발"** 워크로드 선택.

### 1-2. Qt 6.7 이상 (MSVC 2022 64-bit)

[qt.io/download-qt-installer](https://www.qt.io/download-qt-installer) → 온라인 설치.

**필요한 모듈** (설치기에서 체크):

| 모듈 | 왜 |
|---|---|
| Qt 6.11.x **MSVC 2022 64-bit** | 본체 |
| Qt Multimedia | `QMediaPlayer` 폴백 백엔드 |
| Qt Shader Tools | NV12→RGB 셰이더 컴파일 (`qt_add_shaders`) |

`Core`·`Widgets`·`Gui`·`Network` 는 본체에 포함된다.

> ⚠ **Qt MQTT는 필요 없다.** 오픈소스 설치기에 아예 안 나오고(Qt for Automation
> 소속), 찾느라 시간 쓸 필요 없다. 이 프로젝트는 libmosquitto를 쓴다.

### 1-3. mosquitto — ★ 이걸 빠뜨리면 MQTT가 안 된다

```bash
winget install EclipseFoundation.Mosquitto
```

수동 설치: [mosquitto.org/download](https://mosquitto.org/download/) →
`mosquitto-2.x.x-install-windows-x64.exe`

**설치 후 확인** (Git Bash):
```bash
ls "/c/Program Files/mosquitto/devel/"
```

`mosquitto.h` 와 `mosquitto.lib` 가 보여야 한다. 안 보이면 설치 마법사에서
개발용 파일 항목이 빠진 것이다.

**이 설치가 세 가지를 준다:**

| 얻는 것 | 용도 |
|---|---|
| `devel/mosquitto.h`, `.lib` | 빌드 |
| DLL 6종 (`mosquitto.dll` 등) | 실행 — CMake가 exe 옆으로 자동 복사 |
| `mosquitto_pub` / `mosquitto_sub` | **디버깅에 매우 유용** (5절 참조) |

> 로컬 브로커 서비스도 함께 깔린다. **쓰지 않아도 된다** — VMS는 RPi B의
> 브로커에 붙는다. 신경 쓰지 말고 두면 된다.

### 1-4. GStreamer (권장 — 없으면 영상 지연 1초+)

[gstreamer.freedesktop.org](https://gstreamer.freedesktop.org/download/) 에서
**MSVC 64-bit** 판 **두 개를 모두** 설치:

- `gstreamer-1.0-msvc-x86_64-*.msi` (runtime)
- `gstreamer-1.0-devel-msvc-x86_64-*.msi` (**development** — 이게 있어야 빌드에 잡힌다)

설치 시 **Complete** 선택. 설치기가 환경변수를 만들어 준다:
```
GSTREAMER_1_0_ROOT_MSVC_X86_64=C:\Program Files\gstreamer\1.0\msvc_x86_64
```

확인:
```bash
echo $GSTREAMER_1_0_ROOT_MSVC_X86_64
```

> 없어도 빌드·실행은 된다 (`QMediaPlayer` 폴백). 다만 지연이 1초 이상이라
> 실사용에는 GStreamer가 필요하다.

### 1-5. PostgreSQL — **필요 없다**

`v6` 부터 VMS는 DB에 붙지 않는다. `libpq.dll` 도, Qt SQL 플러그인도 쓰지 않는다.
(예전 문서를 보고 설치하려 했다면 하지 않아도 된다)

---

## 2. `credentials.ini` 만들기

### 위치 — ⚠ `Local` 이다

```
C:\Users\<내계정>\AppData\Local\GuardX\credentials.ini
```

**`Roaming` 이 아니다.** Qt의 `GenericConfigLocation` 이 Windows에서 `AppData\Local`
을 가리킨다. 헷갈리기 쉬운 부분이고, 실제로 여기서 한참 헤맸다.

폴더가 없으면 만든다:
```bash
mkdir -p "$LOCALAPPDATA/GuardX"
```

### 내용

```ini
[camera]
host=192.168.0.3
user=admin
password=여기에_카메라_비밀번호

[mqtt]
host=172.20.33.251
port=1883
```

| 섹션 | 필수? | 비고 |
|---|---|---|
| `[camera]` | **필수** | 없거나 비면 앱이 시작하다 종료한다 (fail-closed) |
| `[mqtt]` | 선택 | 코드 기본값이 이미 RPi B(`172.20.33.251:1883`)다. 망이 다르면 여기서 덮어쓴다 |
| `[database]` | **불필요** | `v6` 에서 제거. 남아 있어도 무시된다 |

### 주의 — 저장했는지 확인

Windows 11 메모장은 **저장 안 한 내용을 탭에 유지**한다. 탭 제목에 `●` 표시가
있으면 아직 파일에 안 쓰인 것이다. `Ctrl+S` 를 눌러라.

확인 (값 길이만 출력 — 비밀번호는 안 보인다):
```bash
awk -F= '/^\[/{print} /=/{v=substr($0,index($0,"=")+1); gsub(/^[ \t\r]+|[ \t\r]+$/,"",v); printf "  %-10s = <%d자>\n",$1,length(v)}' "$LOCALAPPDATA/GuardX/credentials.ini"
```

`user`/`password` 가 `<0자>` 면 비어 있는 것이다.

### 비밀번호 암호화 (권장)

한 번 실행해두면 평문이 `dpapi:...` 로 바뀌고, **그 Windows 계정에서만** 풀린다.

```bash
./gstream_VMS.exe --encrypt-credentials
```

---

## 3. 빌드

### 3-1. 클론 + 브랜치

```bash
git clone https://github.com/sinzzangu/7th_VEDA_GROUP2.git
```
```bash
cd 7th_VEDA_GROUP2
```

⚠ **리포 안에서 다시 `git clone` 하지 마라** — 중첩 폴더가 생긴다.
`main` 에 이미 MQTT 작업이 머지돼 있으므로 브랜치를 바꿀 필요도 없다.

### 3-2. Qt Creator에서 열기

`vms/CMakeLists.txt` 를 Qt Creator로 열고 킷을 **Desktop Qt 6.11.x MSVC2022 64bit**
로 선택 → 빌드.

### 3-3. ★ CMake 출력을 반드시 확인

**"일반 메시지"(General Messages) 창에서 이 줄들을 찾아라.**

| 나와야 하는 것 | 뜻 |
|---|---|
| `libmosquitto 발견 — MQTT 활성화` | ✅ 정상 |
| `GStreamer 발견 — 저지연 GPU 백엔드 활성화` | ✅ 정상 |

| 나오면 문제 | 조치 |
|---|---|
| `libmosquitto 미발견 — MQTT는 무동작 스텁으로 빌드됨` | mosquitto 설치 후 **CMake 다시 실행** |
| `GStreamer 미발견` | GStreamer development 판 설치 |

⚠ **mosquitto를 나중에 설치했다면 재빌드만으론 안 잡힌다.**
Qt Creator → **빌드 → CMake 다시 실행** → 그다음 재빌드.

---

## 4. 실행 후 확인 — 로그 4줄

Qt Creator **애플리케이션 출력**에서:

```
[MqttLink] 시작 — "172.20.33.251" : 1883 client_id "vms-<PC이름>"
[MqttLink] 브로커 접속됨
[MqttLink] 구독: "guardx/db/rpib/zones" (qos 1 )
[ZoneConfig] 구역 설정 4 건 수신 — 정원 30 10 10 20
```

### 증상별 진단

| 증상 | 로그 | 원인 |
|---|---|---|
| 앱이 안 뜸 + 메시지박스 | `[camera] user/password가 비어 있습니다` | `credentials.ini` (2절) |
| 영상 O, **정원·히트맵 X** | `libmosquitto 없이 빌드됨 — MQTT 비활성` | **mosquitto 미설치** (1-3) |
| 영상 O, 정원·히트맵 X | `시작 — "192.168.0.3"` | `[mqtt] host` 가 카메라를 가리킴 |
| 영상 O, 정원·히트맵 X | `시작` 은 있고 `브로커 접속됨` 없음 | 네트워크 / RPi B 폴러 정지 |
| `pthreadVC3.dll이 없어...` | — | CMake 재실행 후 재빌드 |
| 영상만 검음 | `GStreamer 미발견` | GStreamer 설치 |

### 화면 확인 (좌측 메뉴)

| 화면 | 정상이면 |
|---|---|
| **LIVE** | 4채널 영상 + `OCC 0/30` 형태 배지 |
| **CROWD** | 우측에 날짜 목록, 날짜 클릭 시 히트맵 |
| **ZONE SETTINGS** | 정원 표 표시, 값 변경 후 [적용] → 1초 내 OCC 반영 |

**OCC 분모가 `60 / 80 / 40 / 30` 으로 보이면 MQTT가 안 오는 것이다** —
그건 코드에 박힌 기본값(`theme.cpp` `channel_cap`)이다.

---

## 5. 문제를 가르는 명령 — mosquitto CLI

VMS를 건드리지 않고 **브로커까지 닿는지** 확인할 수 있다.

```bash
"/c/Program Files/mosquitto/mosquitto_sub.exe" -h 172.20.33.251 -t 'guardx/db/#' -v -W 5
```

| 결과 | 뜻 |
|---|---|
| `guardx/db/rpib/zones {...}` 가 뜬다 | 브로커·폴러 정상 → **VMS 설정 문제** |
| 아무것도 안 뜬다 | 네트워크 문제 또는 **RPi B 폴러 정지** |
| `Error: ...` | 브로커에 못 닿음 |

포트만 확인:
```bash
timeout 3 bash -c "echo > /dev/tcp/172.20.33.251/1883" && echo "열림" || echo "막힘"
```

---

## 6. 알아두면 시간 아끼는 것들

### 6-1. PowerShell은 JSON을 깨뜨린다

```bash
# ❌ PowerShell — 큰따옴표가 사라져 payload가 깨진다
mosquitto_pub -t x -m '{"a":1}'

# ✅ Git Bash 를 쓰거나
# ✅ 파일로 넘긴다
mosquitto_pub -t x -f payload.json
```

Windows PowerShell 5.1은 네이티브 exe에 인자를 넘길 때 문자열 안의 큰따옴표를
삼킨다. 실제로 이걸로 한참 헤맸다.

### 6-2. 헤더를 고쳤는데 이상하게 죽으면 클린 빌드

증분 빌드가 헤더 의존을 놓쳐 다른 `.obj` 가 옛 클래스 크기로 남으면
`new` 가 메모리를 덜 잡아 힙이 깨진다 (`CrowdPage` 생성자에서 크래시한 적 있음).

Qt Creator → **빌드 → 프로젝트 정리** → 재빌드

또는:
```bash
cmake --build <builddir> --clean-first --parallel
```

### 6-3. 빌드 전에 앱을 끌 것

실행 중이면 `LNK1104: gstream_VMS.exe 파일을 열 수 없습니다` 로 링크가 막힌다.

### 6-4. 영상 프로파일(해상도) 바꾸기

레지스트리에 있다. 코드 수정 불필요.

```bash
reg add "HKCU\Software\GuardX\VMS" /v grid_profile /t REG_SZ /d profile4 /f
```

기본값은 `profile5`. 자세한 건 `VMS_CODE_MAP.md` 3-2절.

### 6-5. ★ 글자가 뭉개져 **다른 글자로** 보이면 — 화면 배율 (2026-08-10)

노트북 내장 패널처럼 **125% 배율** 화면에서 한글이 뭉개져 `안전`이 `안석`처럼
보이는 일이 있었다. 외부 모니터(100%)에 꽂으면 멀쩡했다.

**폰트 문제가 아니다.** devicePixelRatio 가 1.25 같은 분수면 위젯이 놓인 위치가
device pixel 경계에 딱 떨어지지 않고, 어긋난 위젯의 글자만 안티에일리어싱 없이
하드 글리프로 그려진다. 그래서 **같은 화면 안에서 어떤 라벨은 멀쩡하고 어떤
라벨만 깨지는**, 폰트·크기·굵기로는 설명이 안 되는 모양이 된다.

지금은 앱이 **배율을 정수로 반올림**해서(1.25 → 1.0) 이 문제를 피한다
(`main.cpp` `apply_dpi_rounding_policy()`, `QApplication` 생성 **전에** 호출).
기동 로그에 `[main] 화면 배율 반올림: round` 가 찍힌다.

⚠ 대가로 125% 화면에서 앱이 그만큼 **작아진다.** 크게 쓰려면 레지스트리로
바꾼다 — 재빌드 불필요:

```bash
reg add "HKCU\Software\GuardX\VMS" /v dpi_rounding /t REG_SZ /d ceil /f
```

| 값 | 125% 화면에서 | 비고 |
|---|---|---|
| `round` (기본) | 1.0 | 선명, 대신 작아짐 |
| `ceil` | 1.5 | 선명하고 커짐 |
| `floor` / `roundpreferfloor` | 1.0 | round 와 사실상 같음 |
| `passthrough` | 1.25 | Qt 기본값 = **글자 깨짐 재현** |

진단법: 증상이 의심되면 환경변수로 먼저 확인한다(코드·레지스트리 안 건드림).

```powershell
$env:QT_SCALE_FACTOR_ROUNDING_POLICY="Round"; .\gstream_VMS.exe
```

### 6-6. 번들 폰트가 실렸는지 로그로 확인 (2026-08-10)

디자인 원본 폰트(IBM Plex)는 `VMS/fonts/*.ttf` 를 **실행 파일 안에** 넣어
쓴다 — PC마다 폰트를 설치할 필요가 없다. 기동 로그에서 확인한다:

```
[Theme] 번들 폰트 6 개 등록: "IBMPlexMono-Regular.ttf, ..."
```

⚠ `[Theme] 번들 폰트 없음 — 시스템 폰트로 폴백` 이 뜨면 리소스 경로가 어긋난
것이다. `CMakeLists.txt` 의 `qt_add_resources(... BASE "fonts")` 에서 `BASE` 가
빠지면 리소스가 `:/fonts/fonts/…` 로 들어가는데, 코드는 `:/fonts` **바로
아래**만 훑어서 한 개도 못 찾는다. 08-03~08-10 사이가 그 상태였고, 폴백이
조용히 동작해서 아무도 몰랐다(그래서 이 로그를 경고로 올려뒀다).

참고: **IBM Plex 에는 한글이 없다.** 한글은 항상 맑은 고딕이 그린다
(`theme.cpp` `korean_family()` 가 명시 지정). 번들 폰트는 라틴·숫자·기호에만
영향을 준다.

---

## 7. 세팅 체크리스트

```
[ ] Visual Studio 2022 + C++ 데스크톱 개발 워크로드
[ ] Qt 6.7+ MSVC2022 64bit (+ Multimedia, Shader Tools)
[ ] mosquitto           → devel/mosquitto.h 확인
[ ] GStreamer runtime + development  → 환경변수 확인
[ ] %LOCALAPPDATA%\GuardX\credentials.ini  ([camera] 채우고 저장)
[ ] 네트워크: 카메라 192.168.0.3, 브로커 172.20.33.251:1883
[ ] CMake 출력에 "libmosquitto 발견", "GStreamer 발견"
[ ] 실행 로그에 "브로커 접속됨"
[ ] LIVE / CROWD / ZONE SETTINGS 세 화면 확인
```

---

## 8. RPi B 쪽이 떠 있어야 한다

VMS만 세팅해도 **폴러가 안 돌면 데이터가 안 온다.**

```bash
ssh <계정>@172.20.33.251 "systemctl is-active guardx-poller"
```

`active` 가 아니면 VMS는 브로커에 남은 **옛 retained 값**만 보여준다 —
정원은 나오는데 히트맵이 안 되는 상태가 이것이다.

폴러 쪽 세팅은 `rpi_b/POLLER_VMS_CHANGES.md` 참조.
