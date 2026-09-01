# rpi_b TODO — RPI B END (2026-07-27 기준, v15 가동 중)

> 카메라 사이드는 `juan_application/TODO.md`. 시스템은 systemd(guardx-poller)
> + cron(guardx_maintain 00:05)으로 상시 가동 중.

## 기능 (우선)

- [x] 🔺 ~~threshold 비교해서 emergency 쏘기 (혼잡 경보 발화기)~~ —
      2026-07-28 배포 완료: `task_alert` (60초 pred 틱, 상태 전이 발화 +
      hysteresis 해제 밴드 warn×0.9, warmup 예측 제외). capacity 전 존 10
      (warn 8명 / critical 9명). **live-fire 실인원 검증 완료 (07-28 오후)**:
      OPEN critical 발화 + MQTT 수신까지 전 사슬 확인
      - **v16.1 (07-28 오후): 3신호 판정** — 현재 인원(now_smoothed, 즉시)
        + 분 중앙값(체류, false-clear 방지 바닥) + 예측 p50 중 나쁜 쪽 채택.
        급증은 다음 60초 틱 안에 발화, 해제는 중앙값 해소까지 신중
- [x] 🔺 ~~MQTT 상황 통지 (transmission layer 연동)~~ — 2026-07-28 완료:
      `mqtt_pub.cpp`(libmosquitto, client id `rpib-poller`), 상태 전이 시
      `guardx/alert/rpib`(잠정 토픽) QoS 1 발행. payload = envelope +
      event/zone_id/channel/incident_id/severity(warn|critical|clear)/count/
      capacity/source. **액추에이터 선택은 팀원 소관 — 상황만 통지.**
      실기기 수신 검증 완료. 규약 문서 전달사항 3건: ① alert 토픽 신설 합의
      ② §3 node_id 발행자/대상 모호 ③ §4-5 camera 토픽 "불요" 회신
      - **결정 (2026-07-28)**: p_over_capacity는 경보에 사용 안 함 — 카메라
        상수 20 기준이라 부정합 + task_alert와 역할 중복. 위험 기반 경보가
        필요해지면 **p90 >= capacity_limit** 판정(폴러 측, DB 용량 기준)으로
        구현 — 카메라 연동 불필요. 컬럼은 모델 사후 평가용으로 유지
- [x] 🔺 ~~스키마 피드백 4호 — congestion_prediction에 p10/p90/
      p_over_capacity/warmup 컬럼 추가~~ — 2026-07-28 코드 완료:
      `migration_prediction_feedback4.sql` + schema.sql 본체 + task_prediction
      INSERT 확장(warmup 행도 플래그 적재, -1→NULL 규칙) + task_alert에
      `warmup IS NOT TRUE` 필터. **남은 절차**: 실기기 migration 적용 →
      재빌드·재시작, 이병규 측 협의 공유
      - **결정 (2026-07-28)**: p_over_capacity는 경보 판단에서 영구 제외
        (카메라 상수 20 기준 + task_alert 중복 — 역할 분리: 카메라=예측
        분위수, rpi_b=임계 판정). 적재는 유지(모델 평가용). 카메라
        zone_thresholds 연동 불필요 — kCapacityLimit 20은 무해한 내부 상수

## 성능 — 폴러 스레딩 검토 결과 (2026-07-28 분석)

> 배경: 단일 스레드 1초 틱 루프라 느린 요청 하나가 전체를 막음.
> 실측 병목: /detections 백로그 1.1MB/11.5s (http_client.cpp 주석) — 이 동안
> det 2s 주기·face·pred·occ 전부 지연. 매 요청 curl_easy_init/cleanup이라
> TLS+Digest(왕복 2회) 핸드셰이크를 요청마다 반복하는 것도 큰 몫.

- [x] 🔺 ~~1순위: CURL persistent handle 재사용~~ — 2026-07-28 코드 완료:
      httpGet 내부 thread_local 핸들 (API 불변, 2-레인 도입 시 레인별 핸들
      자동). + CAM_CAINFO 인증서 고정 훅 (설정만으로 TLS 검증 전환 가능).
      실기기 검증 완료 (07-28: CAM_PINNED_KEY 핀 모드로 가동, [det] 정상)
- [ ] **2순위: 2-레인 스레딩 (격리 목적)** —
      fast lane: det(2s)+face(10s) / slow lane: pred·occ·flow(60s)+cfg(30s).
      60s 배치·det 백로그가 서로 안 막게 격리. 필수 조건:
      - `pqxx::connection` 스레드 비안전 → **스레드당 커넥션 1개** (localhost라 부담 없음)
      - `State::save()`가 파일 전체를 씀 → mutex 또는 태스크별 상태 파일 분리
      - `pollFlow`의 static prev 맵은 flow를 한 스레드에 고정하면 안전
      - `sleep_for` → `sleep_until`(절대 마감)로 틱 드리프트 제거
- [ ] ⚠ **하지 말 것: 채널별 pred/occ 요청 병렬화** — 카메라 앱은
      ProcessAEvent 단일 이벤트 루프(HTTP와 5Hz analytics 동일 스레드,
      mutex 전무)라 병렬 요청도 직렬 큐잉됨. 이득 없이 analytics 핫패스만
      밀림. 동시 요청은 2-레인의 최대 2개까지가 안전선.

## 협의·정리

- [x] ~~**[담당: 병규] task_vms dates 쿼리 최적화**~~ — 2026-07-31 완료:
      파티션 카탈로그(`pg_inherits`+`pg_class`) 조회로 교체.
      **1,708ms → 10.2ms** (planning 포함, 폴러 가동 중 3회 중앙값).
      기존 방식은 행 수에 비례해 계속 나빠졌다 (07-29 0.88초 → 07-31 1.69초).
      `day <= current_date`로 선생성 미래 파티션 7개 제외, `EXISTS`로 빈 과거
      파티션 제외 (실측 07-23~26 4일 — 날짜 필터만으로는 못 거른다).
      신·구 쿼리 날짜 집합 일치 3회 확인. 스키마 변경·마이그레이션 없음.
      상세·재현법: `DATES_QUERY_OPTIMIZATION.md`

- [x] ~~PR 생성~~ — 2026-07-28 완료: VEDA-130 → main 병합됨
- [x] ~~migration_zones_unique.sql 적용~~ — 2026-07-28 완료: 실기기 적용,
      `\d zones`에서 uq_zones_camera_channel UNIQUE 확인 (중복 0행 사전 확인)

## 기능 (후순위)

- [ ] (선택) **faces JPEG 아카이브 파이프라인** — image_ref는 카메라 보존을
      따르므로, 얼굴 이미지 영구 보관이 필요해지면 폴러가 JPEG 다운로드→저장
- [ ] **실데이터 백테스트** (8월 초, 실데이터 2주 축적 후) — `model/` 하네스로
      채널별 실전 MAE 판정. zone_occupancy가 쌓이므로 reduce 단계 생략 가능
- [ ] **TASK-15 config_version 재주입** — 카메라 재시작 시 epoch 1 리셋 대응
      (HANDOFF §3-3, ⚠ reset_flow 혼용 금지)

## 보안 (운영 전환/제출 전 필수)

- [x] ~~TLS 검증 + 신원 고정~~ — 2026-07-28 완료: **공개키 핀 모드**
      (`CAM_PINNED_KEY=sha256//…`, CURLOPT_PINNEDPUBLICKEY — 인증서 CN이
      hanwha-security.com 도메인이라 IP 접속은 CAINFO/호스트명 검증 불가 →
      핀이 정답). `setup_security.sh`가 추출·핀 계산·설정 일괄 (멱등,
      재추출 --refresh-cert). 실기기 검증: CAM_INSECURE=0 + [det] 정상
- [x] ~~config.env·camera.pem 600 + systemd 샌드박스~~ — 2026-07-28 완료:
      hardening.conf 드롭인 (ProtectSystem=strict + state·/run/postgresql
      RW 카브아웃, NoNewPrivileges 등). ⚠ 교훈: ReadWritePaths 경로는
      실존해야 함 (미존재 → NAMESPACE 실패로 서비스 기동 불가)
- [x] ~~state 영속 사일런트 고장 수리~~ — 2026-07-28 발견·수리: state/
      디렉터리 미존재로 save()가 무위 → 재시작마다 링 재수신(중복 detections
      7,259건의 진범)·cfg epoch 리셋. state.cpp가 디렉터리 생성+실패 로깅,
      실기기에서 poller_state.json 최초 생성 확인
- [ ] **보안 잔여 (제출·공개 전 필수)**:
  - SSH `PasswordAuthentication no` — 수동 (락아웃 위험, 폴백 세션 열고 작업)
  - 카메라 **폴링 전용 계정** 생성·교체 — 현재 admin 사용, 비밀번호가
    camera_credentials·config.env·docs에 평문
  - DB 계정(guardx_*·juan) 비밀번호 회전 — 실값이 git 이력·문서에 노출됨
  - git 이력·문서의 자격증명 정리 방침 결정 — **제출·공개·발표 전 필수**
  - **MQTT 브로커 인증** — 현재 `listener 1883` + `allow_anonymous true`
    (LAN 전체가 발행 가능 = 가짜 경보/액추에이터 명령 주입 가능). 최소
    password_file, 정석은 규약이 예고한 mTLS(client CN=노드ID). 팀 공동 작업
    (A·C 클라이언트 동시 수정 필요)

## 모니터링 (주 1회)

- [ ] `/var/log/guardx_maintain.log` — 7항목 요약 정상 실행
- [ ] detections 파티션 크기 — Face/Head 합류로 ~3배 유입, 일 500MB 초과 시
      cron을 `guardx_maintain(7, …)`으로
