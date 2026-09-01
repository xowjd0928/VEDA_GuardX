# 바닥평면 캘리브레이션 조사 — OpenSDK 구조 · 기존 구현 · 알고리즘 · 설계

작성 2026-08-04 · 대상 PNM-C16083RVQ (CV5, 4채널) · SDK 26.05.19

목적: 카메라 화면의 객체를 **하나의 floor plan 위 2D 좌표로 찍는** 구조를 만든다.
이 문서는 그 전제가 되는 ① SDK가 무엇을 주는가 ② 팀이 이미 무엇을 만들었나
③ 학계·OpenCV가 제공하는 수단 ④ 어떻게 조립할 것인가를 정리한다.

---

## 1. OpenSDK — 앱을 어디서 어떻게 고치는가

### 1.1 실행 모델

Open Platform은 **컴포넌트 + 이벤트** 구조다. 앱은 `main()`을 갖지 않는다.
`life_cycle_manager` 바이너리가 앱 프로세스가 되고, 우리가 만든 컴포넌트를
`.so`로 동적 로드해 이벤트를 배달한다.

| 개념 | 의미 |
|---|---|
| Container | 앱 프로세스 (= LCM). `PLifeCycleManagermanifest.json`이 정의 |
| Component | 기능 단위 `.so`. `Component`를 상속하고 `create_component`를 export |
| Event | 컴포넌트 간·플랫폼과의 통신 단위. 타입 정수로 분기 |
| Association | 누구에게서 받고(`SourceNames`) 누구에게 보내는지(`ReceiverNames`) |

컴포넌트가 구현할 것은 사실상 두 개뿐이다.

```cpp
bool Initialize();               // URI 등록, 상태 복원
bool ProcessAEvent(Event* e);    // switch (e->GetType()) 로 전부 분기
```

### 1.2 앱이 받을 수 있는 것 (Input Events)

바닥 캘리브레이션에 직접 관계있는 것만:

| 이벤트 | 내용 | 쓸모 |
|---|---|---|
| **ONVIF Metadata** (`IPMetadataManager::eMetadataRequest`) | 카메라 내장 WiseAI가 만든 analytics XML. 채널 번호 포함 | ★ 객체 박스·ID를 **공짜로** 준다 |
| **Raw Video** (`IPVideoFrameRaw`) | `chan_id, seq, pts, format, width, height` + 원본 프레임 | 직접 OpenCV 돌릴 때 |
| Motion / Tampering / Defocus 등 | 내장 analytics 결과 | 보조 |
| Setting Changed | 라인·IVA 설정 변경 push | 형상 바뀌면 리셋 |

**이게 핵심 판단 지점이다.** 사람 검출을 직접 할 필요가 없다. ONVIF Metadata가
이미 `ObjectId`(추적 ID) + `BoundingBox`(픽셀) + `Type`(Human/Face/Head)을 5Hz로 준다.
남는 일은 **기하 변환뿐**이다.

### 1.3 앱이 내보낼 수 있는 것 (Output Events)

`Metadata Event`(Set Schema → Send Metadata)로 **자체 메타데이터를 ONVIF 스트림에
얹을 수 있다.** Event / Frame / Dynamic 세 종류. 그 외 Alarm, Snapshot JPEG,
OSD Message, Write Event Log, FTP/Email.

→ 바닥 좌표를 계산해 **Frame Metadata로 실어 보내면 VMS가 RTSP만으로 받는다.**
(현재 팀 구현은 이 경로 대신 HTTP 폴링을 쓴다 — §2.2)

### 1.4 매니페스트 3종

| 파일 | 역할 |
|---|---|
| `config/app_manifest.json` | 앱 이름·버전·**Permission**·**ChannelType** |
| `app/src/<comp>/manifests/*_manifest.json` | `.so` 파일명, DynamicLoadable |
| `..._manifest_instance_N.json` | ★ **인스턴스 + 이벤트 구독** |

**정정 사항 — Permission은 `[SDCard]` 하나뿐이다.** 영상이나 메타데이터 수신에는
권한 선언이 필요 없다. `"Permission": []`로 두어도 ONVIF Metadata는 정상 수신된다.
(팀 앱이 그 상태로 동작 중인 것이 증거)

**`ChannelType`이 4채널의 열쇠다.**

| 값 | 동작 |
|---|---|
| `Single` | 단일 채널만 |
| `Multiple` | 다중 채널 |
| **필드 생략** | 다중 채널 = **전 채널 사용** |

구독은 인스턴스 매니페스트의 `SourceNames`에서 채널별로 나열한다:

```json
"SourceNames": [
  { "Source": "MetadataManager_0", "GroupName": "Metadata::OpenApp" },
  { "Source": "MetadataManager_1", "GroupName": "Metadata::OpenApp" },
  { "Source": "MetadataManager_2", "GroupName": "Metadata::OpenApp" },
  { "Source": "MetadataManager_3", "GroupName": "Metadata::OpenApp" },
  { "Source": "MetadataManager_4", "GroupName": "Metadata::OpenApp" },
  { "Source": "OpenPlatform",      "GroupName": "SettingChange" }
]
```

`MetadataManager_0..4`를 전부 걸고, 수신 측에서 `param.channel()`로 갈라 쓴다.
**test_calibration에 그대로 복사하면 4채널이 열린다.**

### 1.5 OpenCV가 이미 SDK 안에 있다

```
SampleApplication/webservice_sample/app/3rd-party/opencv-3.4.20/
```

**OpenCV 3.4.20이 번들되어 있다.** `cv::findHomography`, `cv::fisheye`,
`cv::calibrateCamera`, `cv::perspectiveTransform` 전부 카메라 위에서 쓸 수 있다.
관련 샘플: `display_image_opencv`, `run_neural_network`,
`high_fps_rawvideo_receive`, `send_metadata`, `snapshot_jpeg`.

> ⚠️ 3.4.x는 `cv::fisheye`는 있지만 4.x의 일부 신규 API는 없다. 호스트에서
> 4.x로 검증한 코드를 옮길 때 API 차이를 확인할 것.

---

## 2. 팀 구현(`juan_application`) 분석 — 무엇을 어떻게 바꿨나

### 2.1 SDK 접점은 딱 4곳

| # | 접점 | 코드 위치 |
|---|---|---|
| 1 | `eMetadataRequest` 수신 → ONVIF XML 파싱 | `metadata_probe.cc:117` |
| 2 | `OpenAPIRegistrar`로 HTTP URI 13개 등록 | `metadata_probe.cc:95` |
| 3 | `eHttpRequest` 수신 → 라우팅 | `probe_http.cc` |
| 4 | `eAnalyticsSettingChanged` → 모델 리셋 | `metadata_probe.cc:187` |

**나머지는 전부 순수 C++ 로직이다.** SDK는 "XML 받고 HTTP 열어주는" 얇은 껍질로만
쓰고, 그 안에서 파싱·통계·예측을 직접 구현했다.

### 2.2 index.html 관련 — 오해 정정

`app/html/index.html`은 **SDK 스켈레톤 그대로이고 손대지 않았다.** (WriteEventLog /
CheckTimeSetting 데모 버튼 두 개뿐, 2.2KB)

메타데이터를 밖으로 내보내는 실제 경로는 **OpenAPI 등록**이다.

```cpp
auto* reg = new ("OpenAPI") IAppDispatcher::OpenAPIRegistrar(
                String(u), GetInstanceName(), methods);
SendNoReplyEvent("AppDispatcher",
                 (int32_t)IAppDispatcher::EEventType::eRegisterCommand, 0, reg);
```

등록된 URI는 이 주소로 노출된다:

```
http://<카메라IP>/opensdk/<app_id>/<uri>
```

index.html이 의미 있는 건 `base_uri` 규칙을 보여준다는 점 하나다:

```js
const app_id = window.location.pathname.split('/')[5];
const base_uri = `${location.protocol}//${location.hostname}:${location.port}/opensdk/${app_id}`;
```

즉 **웹뷰어 페이지가 아니라 HTTP 엔드포인트가 인터페이스다.** 웹 UI를 붙이고
싶으면 index.html에서 자기 앱의 엔드포인트를 `fetch`하면 된다.

### 2.3 등록된 엔드포인트 13개

```
/metadata /samples /events /stats /capture /bestshots /prediction
/occupancy /forecast_day /detections /faces /config /captest
```

소비 구조는 **카메라가 push하지 않고 RPi B 폴러가 pull**한다
(`/detections` 2초, `/faces` 10초, `/prediction`·`/occupancy`·`/events` 60초).

### 2.4 ★ 우리가 쓸 좌표 데이터

`ParseFrame()`이 `<tt:Object>`마다 뽑는 것:

| 필드 | 출처 | 의미 |
|---|---|---|
| `ObjectId` | 속성 | **추적 ID** (프레임 간 유지) |
| `Parent` | 속성 | Face/Head → 소속 사람 ID |
| `Type` | `<tt:Type>` | Human / Face / Head |
| `cx, cy` | `<tt:CenterOfGravity>` | 무게중심 (float) |
| `rsx,rsy,rex,rey` | `<tt:BoundingBox>` | **left/top/right/bottom (픽셀 정수)** |

`/detections` JSON으로 그대로 나온다:

```json
{"channel":1,"object_id":415,"category":1,"likelihood":0.92,
 "rect_sx":..,"rect_sy":..,"rect_ex":..,"rect_ey":..,
 "x":..,"y":..,"ts":"2026-..Z"}
```

**발밑점(foot point) = `((rect_sx+rect_ex)/2, rect_ey)`** — 이것이 호모그래피의
입력이다. 무게중심(`x,y`)이 아니라 **박스 아랫변 중앙**을 써야 한다. 사람의
바닥 접지점이기 때문이다.

### 2.5 예측 모델

`hw_forecaster.hpp` — Holt-Winters damped (`model_version='hw_damped_v1'`).
1분 버킷 중앙값을 관측으로 넣고 5/30/60/180분 horizon의 p50/p10/p90을 낸다.
프레임이 없는 분은 0으로 backfill(추적 객체가 없을 때만 프레임이 오므로).
**군중흐름 예측은 별도 AI가 아니라 시계열 통계 모델이다.**

---

## 3. 바닥평면 캘리브레이션 — 이론 · 논문 · API

### 3.1 문제의 성질

바닥은 **평면**이다. 평면 위의 점은 이미지와 world 사이에 **호모그래피(3×3, 자유도 8)**
로 정확히 대응한다. 카메라 높이·틸트·초점거리를 몰라도 된다.

$$s\begin{bmatrix}X\\Y\\1\end{bmatrix} = H \begin{bmatrix}u\\v\\1\end{bmatrix}$$

**대응점 4쌍이면 유일하게 결정된다** (점 하나당 제약 2개 × 4 = 8 = 자유도).
바닥 위에 있는 점이어야 한다는 것만 지키면 된다.

⚠️ 단, 호모그래피는 **렌즈 왜곡이 없다고 가정**한다. 이 카메라는 광각이라
왜곡 보정이 선행되어야 한다 (§3.3 1단계).

### 3.2 표준 파이프라인 4단계

```
① 왜곡 보정        cv::fisheye::undistortImage  (또는 initUndistortRectifyMap)
        ↓ 직선이 직선으로 보이는 이미지
② 호모그래피 추정   cv::findHomography(img_pts, world_pts, RANSAC)
        ↓ H (채널당 1개)
③ 발밑점 투영      cv::perspectiveTransform( ((sx+ex)/2, ey) , H )
        ↓ (X, Y) cm — floor plan 좌표
④ 멀티채널 융합     겹침 구역 중복 제거 + 트랙 이어붙이기
        ↓ 하나의 지도
```

### 3.3 단계별 OpenCV API

**① 왜곡 보정**

| 함수 | 용도 |
|---|---|
| `cv::fisheye::calibrate` | 체스보드 여러 장 → K, D (광각/어안 전용) |
| `cv::calibrateCamera` | 일반 렌즈용. **어안엔 실패하거나 크게 부정확** |
| `cv::fisheye::initUndistortRectifyMap` + `remap` | 실시간 보정 (map 1회 생성 후 재사용) |
| `cv::fisheye::undistortPoints` | ★ **이미지 전체 대신 점만 보정** — 훨씬 싸다 |

> 우리는 이미지를 볼 필요가 없고 **박스 좌표만 필요**하므로 `undistortPoints`가
> 정답이다. 프레임 remap은 CPU를 크게 먹는데 안 해도 된다.

목표 재투영 오차 **0.5px 미만**.

**② 호모그래피**

| 함수 | 비고 |
|---|---|
| `cv::findHomography(src, dst, cv::RANSAC, 3.0)` | 4점 초과 시 최소자승 + 이상치 제거. **점을 많이 넣을수록 좋다** |
| `cv::getPerspectiveTransform` | 정확히 4점. RANSAC 없음 |

**③ 투영**

`cv::perspectiveTransform` — 점 배열을 한 번에. 역방향은 `H.inv()`.

**④ 검증**

`cv::warpPerspective`로 격자를 되쏘아 눈으로 확인. **RMS 숫자만으로 판정하지 말 것**
(팀 `calib_compare.py` 주석에 이미 정확히 지적되어 있음).

**★ 보너스: `cv::aruco`가 SDK 번들에 있다**

기준점을 손으로 클릭하는 대신 **ArUco 마커로 자동 검출**할 수 있다.

| 방법 | 검출 정확도 | 수고 |
|---|---|---|
| 손 클릭 (현재) | 클릭 오차 수 px | 중간 |
| **ArUco 마커** | **서브픽셀 · 자동** | A4 인쇄 + 부착 |

A4에 마커 4~6장을 인쇄해 바닥에 놓고 위치만 실측하면 **클릭 오차 문제가 통째로
사라진다.** `cv::aruco::detectMarkers()` 한 줄이고, 마커 ID가 붙어 있으니
**어느 점이 어느 world 좌표인지 헷갈릴 일도 없다.** 겹침 구역 검증에서도
"두 채널이 같은 마커를 어떻게 보는가"로 바로 비교된다.

`ccalib`(omnidirectional 모델)도 번들에 있어, 왜곡이 `fisheye` 모델로 안 잡히면
대안이 된다.

### 3.4 참고 논문 — 분류별

**(A) 기본 이론**

- **Criminisi, Reid, Zisserman, "Single View Metrology", IJCV 2000**
  — 단일 뷰에서 소실선·소실점만으로 아핀 구조를 얻는 고전. V1(선 기반) 방식의
  이론적 근거. [PDF](https://www.cs.cmu.edu/~ph/869/papers/Criminisi99.pdf)
  · [Springer](https://link.springer.com/article/10.1023/A:1026598000963)

**(B) 자동 캘리브레이션 — 사람을 자로 쓰기**

- **Camera auto-calibration using pedestrians and zebra-crossings**
  — 보행자의 머리-발 선분이 3D에서 평행 → 수직 소실점 추정.
  [Academia](https://www.academia.edu/16959323/Camera_auto_calibration_using_pedestrians_and_zebra_crossings)
- **Simultaneous surveillance camera calibration and foot-head homology estimation
  from human detections** — 사람 검출만으로 foot-head homology를 세운다.
  [ResearchGate](https://www.researchgate.net/publication/221361622_Simultaneous_surveillance_camera_calibration_and_foot-head_homology_estimation_from_human_detections)
- **Efficient height measurements in single images based on the detection of
  vanishing points**
  [ScienceDirect](https://www.sciencedirect.com/science/article/abs/pii/S1077314215000855)
- **Autocamera Calibration for traffic surveillance cameras with wide angle lenses**
  — 광각 렌즈 + 자동 캘리브레이션. 우리 상황과 렌즈 조건이 가깝다.
  [arXiv](https://arxiv.org/pdf/2001.07243)

→ **팀의 `calib_auto_pedestrian.py`가 정확히 이 계열이다.** 문헌적으로 근거 있는
접근이고, 스크립트 주석의 "대체재가 아니라 검산용" 판단도 문헌과 일치한다.

**(C) 광각/어안 + 지면 호모그래피**

- **Robust ground plane induced homography estimation for wide angle fisheye
  cameras** (IEEE) — 어안에서 지면 호모그래피를 강건하게.
  [IEEE](https://ieeexplore.ieee.org/document/6856402/)
- **Estimating fisheye camera parameters from homography**
  [Springer PDF](https://link.springer.com/content/pdf/10.1007/s11432-011-4277-9.pdf)
- **Analytical Modeling and Correction of Distance Error in Homography-Based
  Ground-Plane Mapping** — ★ 호모그래피 매핑의 **거리 오차를 정량 모델링하고 보정**.
  정확도 평가에 직접 쓸 수 있다. [arXiv](https://arxiv.org/pdf/2604.10805)
- **Homography-based ground plane detection using a single on-board camera**
  [UPM PDF](https://oa.upm.es/10759/2/INVE_MEM_2010_96402.pdf)

**(D) 멀티카메라 융합 — floor plan 위 통합**

- **MVDet: Multiview Detection with Feature Perspective Transformation**
  — 각 뷰의 **특징맵**을 지면에 투영해 합친 뒤 검출. WILDTRACK 88.2% MODA.
- **EarlyBird: Early-Fusion for Multi-View Tracking in the Bird's Eye View**
  [arXiv](https://arxiv.org/pdf/2310.13350)
- **TrackTacular: Lifting Multi-View Detection and Tracking to the BEV**, CVPRW 2024
  [arXiv](https://arxiv.org/abs/2403.12573)
  · [CVF PDF](https://openaccess.thecvf.com/content/CVPR2024W/3DMV/papers/Teepe_Lifting_Multi-View_Detection_and_Tracking_to_the_Birds_Eye_View_CVPRW_2024_paper.pdf)
- **WILDTRACK 데이터셋** — 7대 동기화 카메라, 12×36m, 지면 2.5cm 격자 GT.
  **평가 방식이 중요하다: IoU가 아니라 지면 좌표의 유클리드 거리로 판정한다.**
- **Multicamera edge-computing system for persons indoor location and tracking**
  [ScienceDirect](https://www.sciencedirect.com/science/article/pii/S2542660523002639)
- **Alternatives for Locating People Using Cameras and Embedded AI Accelerators**
  — 엣지 디바이스에서의 실전 구현. [MDPI](https://www.mdpi.com/2673-4591/7/1/53)
- **Measurement-Calibrated Multi-Camera Fusion for Vision-Based Indoor Localization**
  [arXiv](https://arxiv.org/pdf/2606.13509)

> **판단**: MVDet/EarlyBird 계열은 특징맵을 지면에 투영해 **학습**하는 방식이라
> 카메라별 GT와 학습 파이프라인이 필요하다. 우리는 카메라가 이미 검출·추적을
> 해주므로 **(D)의 학습 모델은 불필요**하다. 다만 **평가 지표(지면 유클리드 거리)와
> 겹침 처리 개념**은 그대로 차용할 가치가 있다.

### 3.5 문헌이 경고하는 실패 지점

| 문제 | 원인 | 대응 |
|---|---|---|
| 가까운 사람 오차 폭발 | 발이 프레임 밖 / 머리만 검출 | 평균 키 가정 + 카메라 위치로 발 위치 역산 |
| 지평선 근처 발산 | 원근상 1/(y−y_horizon) → 무한대 | 유효 거리 상한을 두고 그 밖은 버린다 |
| 화면 가장자리 부정확 | 렌즈 왜곡이 가장 큰 영역 | 왜곡 보정 필수 + 가장자리 신뢰도 하향 |
| 겹침 구역 이중 계수 | 두 채널이 같은 사람을 봄 | 지면 좌표 거리 임계로 병합 (§4.4) |

### 3.6 팀이 이미 만든 것에 대한 평가

> **이 스크립트들은 저장소에 없다.** 조사 단계에서 한 번 돌려 판단을 내리려고
> 쓴 일회성 도구였고, 최종 구현(카메라 앱 `test_calibration` + 웹 UI + VMS)의
> 어느 경로에서도 호출하지 않는다. 남길 값어치가 있는 것은 스크립트 자체가
> 아니라 **아래 표의 판단**이므로 그것만 문서로 남겼다. 재현이 필요하면
> "방식" 열의 설명만으로 다시 만들 수 있다.
>
> (`calib_work/`의 작업 이미지와 `points.csv`도 같은 이유로 저장소 밖이다)

| 스크립트 | 방식 | 평가 |
|---|---|---|
| `calib_distortion_check.py` | k1 슬라이더로 왜곡 눈대중 | ✅ 올바른 0단계. "정식 값은 체스보드로" 라는 주석도 정확 |
| `calib_compare.py` V1 | 소실점 + 직각 가정 + 실측 2개 | 가정이 많다. 검산용 |
| `calib_compare.py` V2 | 바닥 사각형 4점 + 실측 | ✅ **이쪽이 정공법.** 자유도와 제약이 정확히 일치 |
| `calib_auto_pedestrian.py` | 보행자 회귀 → 지평선·축척 | ✅ 검산·진단용으로 정확한 포지셔닝 |
| `points.csv` | world 기준점 정의 | ✅ **채널 간 좌표계 통일 원칙이 명시**되어 있음 |

**설계 판단이 전반적으로 문헌과 일치한다.** 남은 격차는 세 가지:

1. **정식 왜곡 보정이 아직 없다** — `calib_distortion_check`는 눈대중이라고
   스스로 밝히고 있다. 체스보드 기반 `cv::fisheye::calibrate`가 필요하다.
2. **기준점이 4~5개로 최소치다** — `points.csv`의 주석이 이미 지적하듯
   벽에서 떨어진 점(가구 다리 접지점 등)을 추가하면 RANSAC이 의미를 갖는다.
   특히 CH2의 4점은 "바닥 표시가 없는 시야 경계"라 오차가 크다.
3. **VMS `to_floor()`가 아직 극좌표 근사다** — `crowd_page.cpp:131`

```cpp
// 화면 세로 = 거리. 프레임 아래쪽일수록 카메라에 가깝다는 원근 근사.
// 정확한 역투영은 카메라 높이·틸트·렌즈 왜곡이 있어야 하므로 여기선 선형.
const double v = qBound(0.0, cam_y / double(FRAME_H), 1.0);
const double r = reach * (fan.near_ratio + (1.0 - v) * (fan.far_ratio - fan.near_ratio));
```

화면 x → 방위각, 화면 y → 거리를 **선형**으로 근사한다. 원근은 선형이 아니므로
멀수록 크게 틀린다. **여기가 호모그래피로 교체될 지점이다.**

> **→ 2026-08-11 15:34 해결됨.** 위 3번(극좌표 근사)은 없어졌다. `to_floor()` 는
> 이제 `calibration.json` 의 호모그래피 H 를 그대로 적용한다. 부채꼴 상수
> (`ChannelFan`·`FANS[]`)와 하드코딩 가구 목록(`ITEMS[]`)도 함께 삭제됐다.
> 구축 절차 전체는 [PIPELINE_CALIBRATION.md](PIPELINE_CALIBRATION.md) 참고.
>
> 1번(정식 왜곡 보정)과 2번(기준점 수)은 **그대로 남아 있다.**

---

## 4. 설계 — 4채널을 하나의 floor plan에 올리기

### 4.1 좌표계 (points.csv 규약 유지)

```
원점    지도 좌상 모서리 (두 벽이 바닥에서 만나는 곳)
X       위쪽 벽을 따라 오른쪽 (+),  0 ~ 1500 cm
Y       왼쪽 벽을 따라 아래   (+),  0 ~ 700 cm
단위    cm (정수 취급 가능)
```

**전 채널이 이 하나의 좌표계를 공유한다.** points.csv 주석의 경고 —
"오른쪽 벽에서 쟀더라도 X를 뒤집지 말 것, 우상 모서리는 (1500, 0)" — 이
원칙이 멀티채널 통합의 전제다.

### 4.2 변환을 어디서 할 것인가

| 위치 | 장점 | 단점 | 판정 |
|---|---|---|---|
| **카메라 앱** | 지연 최소, 메타데이터에 실어 전송 | H를 카메라에 배포·갱신해야 함, 앱 재빌드 | 나중 |
| **RPi B 폴러** | H 변경이 즉시 반영, 재계산 쉬움, 파이썬 | 폴링 주기(2초)에 묶임 | ✅ **1단계** |
| **VMS** | 화면과 가까움 | 소비자마다 중복 구현 | 표시 전용 |

**권장: 폴러에서 변환해 DB에 world 좌표를 함께 적재한다.**

이유는 **H가 자주 바뀌기 때문**이다. 캘리브레이션은 기준점을 늘리고 왜곡 보정을
개선하며 여러 번 갱신된다. 카메라 앱에 넣으면 그때마다 `.cap` 재빌드·재설치인데,
폴러에 두면 설정 파일 교체로 끝난다. 정확도가 수렴한 뒤에 카메라로 내리면 된다.

원본 픽셀 좌표는 **반드시 함께 보존한다.** H가 틀린 것으로 판명됐을 때 과거
데이터를 재투영할 수 있어야 한다.

### 4.3 DB 스키마 제안

`detections`에 두 컬럼 추가 (기존 `geom`=픽셀 무게중심은 유지):

```sql
ALTER TABLE detections
  ADD COLUMN floor_x  REAL,   -- cm, 지도 원점 기준
  ADD COLUMN floor_y  REAL,
  ADD COLUMN calib_id SMALLINT;  -- 어느 H로 계산했나 (재투영·감사용)
```

`calib_id`가 중요하다. `zone_geometry_history`가 형상 이력을 남기는 것과 같은
이유 — **H가 바뀌면 그 전후 데이터는 다른 좌표계**다. 캘리브레이션 이력 테이블:

```sql
CREATE TABLE calibration (
  calib_id    SMALLSERIAL PRIMARY KEY,
  channel     SMALLINT NOT NULL,
  h_matrix    REAL[9] NOT NULL,     -- 행 우선 3x3
  k_matrix    REAL[9],              -- 왜곡 보정 K
  d_coeffs    REAL[4],              -- fisheye D
  rms_px      REAL,                 -- 재투영 오차
  n_points    SMALLINT,             -- 기준점 개수
  valid_from  TIMESTAMPTZ NOT NULL,
  valid_to    TIMESTAMPTZ,          -- 열린 행 = 현재
  note        TEXT
);
```

### 4.4 멀티채널 융합

**CH3·CH4가 지금 다른 곳을 본다는 점이 오히려 유리하다.** 겹침이 없으면 융합
문제 자체가 없다. 채널별로 독립 투영해 그대로 합치면 된다.

겹침이 생기는 경우(CH1↔CH2의 X 600~700 구역)의 처리:

```
1. 각 채널 검출을 world 좌표로 투영
2. 같은 시각(±100ms) 창에서 채널 간 쌍 거리 계산
3. 거리 < 임계(예: 60cm) → 동일 인물로 병합
   - 좌표: 캘리브레이션 신뢰도(재투영 오차, 화면 중심 거리) 가중 평균
   - ID:   전역 track_id 발급, 채널별 object_id는 매핑 테이블에 보존
4. 임계 밖 → 별개 인물
```

임계값은 **겹침 구역에서 한 사람이 걸어갈 때 두 채널의 좌표 차이를 실측해서**
정한다. points.csv의 `P_OVERLAP` 주석이 정확히 이 검증을 예고하고 있다 —
"CH1의 H와 CH2의 H가 같은 답을 내는지만 보면 된다".

### 4.5 정확도 검증 방법

**절대 하지 말 것: RMS 숫자만 보고 판정하기.** 기준점에 과적합된 H도 RMS는 작다.

| 방법 | 판정 |
|---|---|
| 1m 격자 되쏘기 | 바닥 무늬·가구 접지점과 맞는가 (`calib_compare.py`가 이미 함) |
| Leave-one-out | 기준점 하나를 빼고 H를 구해 그 점을 예측 → 실제와의 cm 오차 |
| 겹침 구역 교차검증 | CH1 좌표 vs CH2 좌표의 차이 (GT 불필요) |
| 보행 궤적 | 직선으로 걸으면 지도 위에서도 직선인가 |
| 보행자 회귀 | `calib_auto_pedestrian.py`의 지평선과 H의 지평선이 일치하는가 |

WILDTRACK 관례를 따라 **평가 단위는 지면 유클리드 거리(cm)**로 통일한다.

### 4.6 단계별 로드맵

| 단계 | 할 일 | 산출물 | 상태 |
|---|---|---|---|
| 0 | 왜곡 진단 | k1 눈대중 | ✅ 완료 |
| 1 | **체스보드 왜곡 보정** | 채널별 K, D | ⬜ 다음 |
| 2 | 기준점 보강 (ArUco 마커 or 벽 외 접지점) | points.csv 확장 | ⬜ |
| 3 | 채널별 H 확정 + LOO 검증 | `calibration` 행 4개 | ⬜ |
| 4 | 폴러에 투영 로직 + DB 컬럼 | floor_x/floor_y 적재 | ⬜ |
| 5 | `to_floor()`를 H 조회로 교체 | VMS 실좌표 표시 | ⬜ |
| 6 | 겹침 융합 + 전역 track_id | 채널 넘는 동선 | ⬜ |
| 7 | (선택) 카메라 앱으로 내리기 | Frame Metadata 전송 | ⬜ |

**1단계가 가장 급하다.** 왜곡이 남아 있으면 기준점을 아무리 늘려도 H가 수렴하지
않는다. 체스보드를 인쇄해 각 채널 시야에서 10~20장 찍고 `cv::fisheye::calibrate`를
돌리는 것이 순서상 먼저다.

---

## 부록: test_calibration에 4채널 여는 법

`app/src/sample_component/manifests/SampleComponent_manifest_instance_0.json`의
`SourceNames`에 §1.4의 `MetadataManager_0..4` 6줄을 넣고,
`ProcessAEvent`에 `IPMetadataManager::EEventType::eMetadataRequest` 케이스를
추가하면 4채널 ONVIF 메타데이터가 들어온다. `config/app_manifest.json`의
`ChannelType`은 생략(=전 채널) 상태를 유지한다.
