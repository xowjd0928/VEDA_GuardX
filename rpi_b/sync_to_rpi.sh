#!/usr/bin/env bash
# sync_to_rpi.sh — 개발 VM → RPi B 코드 동기화 + 재빌드 + 서비스 재시작
#
# 사용: rpi_b 루트에서
#   ./sync_to_rpi.sh              동기화 + 빌드 + 재시작
#   ./sync_to_rpi.sh --dry-run    전송 목록만 미리보기 (아무것도 안 바꾼다)
#   ./sync_to_rpi.sh --no-restart 동기화 + 빌드까지만
#   ./sync_to_rpi.sh --no-build   동기화만 (빌드·재시작 안 함)
#   ./sync_to_rpi.sh --force      스테일 가드 무시 (마지막 수단)
#
# 전제: ssh 키 등록 권장 — ssh-copy-id juan@$RPI_HOST
#
# ⚠ RPi 쪽 로컬 파일은 건드리지 않는다: config.env(장비별 설정)·camera.pem
#   (TLS 핀)·state/(커서)·build/(산출물)는 exclude — rsync --delete 는 exclude
#   대상을 삭제하지 않는다.
#
# ── 2026-08-03 전면 개정 ──────────────────────────────────────────────
# ① 진실원천이 뒤집혔다. 예전엔 "공유폴더=편집 원천, git=사본"이었지만 rpi_b
#    작업은 이제 git 에서 한다. 공유폴더는 **배포 스테이징**일 뿐이고, git 에서
#    갱신해 쓴다(.synced_from_git 스탬프). Windows 에 rsync 가 없어 이 스크립트는
#    VM 에서 돌아야 하고, VM 은 git 클론을 못 보기 때문에 이 구조가 됐다.
# ② --delete 로 Pi 코드를 지우는 스크립트라 스테일 트리를 밀면 사고가 난다.
#    실제로 갱신 전 공유폴더는 VEDA-155·task_tracks 가 통째로 없었다 —
#    그대로 밀었으면 Pi 의 동선 파이프라인과 mqttd 가 삭제됐다. 아래 가드가 그것.
# ③ 서비스가 둘이 됐다 (VEDA-155): guardx-poller + guardx-mqttd.
# ④ 남이 쓰고 있으면 재시작하지 않는다 (§PREFLIGHT).
set -euo pipefail

RPI_HOST="${RPI_HOST:-100.73.217.52}"
RPI_USER="${RPI_USER:-juan}"
DEST="~/7th_VEDA_GROUP2/rpi_b/"
SRC="$(cd "$(dirname "$0")" && pwd)/"
SERVICES=(guardx-poller guardx-mqttd)

DRY=""; RESTART=1; BUILD=1; FORCE=0
for a in "$@"; do
  case "$a" in
    --dry-run)    DRY="--dry-run" ;;
    --no-restart) RESTART=0 ;;
    --no-build)   BUILD=0; RESTART=0 ;;
    --force)      FORCE=1 ;;
    -h|--help)    sed -n '2,12p' "$0"; exit 0 ;;
    *) echo "unknown arg: $a"; exit 1 ;;
  esac
done

echo "SRC : $SRC"
echo "DEST: $RPI_USER@$RPI_HOST:$DEST"

# ── 스테일 가드 ──────────────────────────────────────────────────────
# --delete 가 붙으므로 "여기 없는 파일은 Pi 에서도 지워진다". 오래된 트리를
# 밀면 조용히 기능이 사라진다(빌드는 통과하는데 코드가 없어지는 식). 그래서
# **지금 반드시 있어야 하는 경로**를 먼저 검사한다. 새 모듈이 생기면 여기 추가.
REQUIRED=(
  src/Poller/task_tracks.cpp          # VEDA-152 동선 추적
  include/Poller/task_tracks.hpp
  src/MqttDb/mqtt_service.cpp         # VEDA-155 서비스 분리
  src/MqttDb/task_vms.cpp
  src/Mqtt/mqtt_pub.cpp
  src/mqtt_main.cpp
  src/MqttDb/task_auth.cpp            # VEDA-185 로그인·세션
  include/MqttDb/task_auth.hpp
  tools/guardx_passwd.cpp             # 시드 비밀번호를 바꿀 유일한 수단
  systemd/guardx-mqttd.service
  CMakeLists.txt
)
missing=()
for f in "${REQUIRED[@]}"; do [ -e "$SRC$f" ] || missing+=("$f"); done
if [ ${#missing[@]} -gt 0 ]; then
  echo
  echo "⛔ 스테일 트리다 — 있어야 할 파일이 없다:"
  printf '   - %s\n' "${missing[@]}"
  echo
  echo "   이대로 밀면 --delete 가 Pi 에서 같은 파일을 지운다."
  echo "   git(main)에서 공유폴더를 먼저 갱신할 것:"
  echo "     git -C <7th_VEDA_GROUP2> archive main rpi_b | tar -x -C /tmp/x"
  echo "     cp -r /tmp/x/rpi_b/. <공유폴더>/rpi_b/"
  [ "$FORCE" = 1 ] || exit 1
  echo "   (--force 지정됨 — 계속한다)"
fi

if [ -f "$SRC.synced_from_git" ]; then
  echo "stamp: $(tr '\n' ' ' < "$SRC.synced_from_git")"
else
  echo "⚠ .synced_from_git 없음 — git 기준으로 갱신된 트리인지 확인 불가"
  [ "$FORCE" = 1 ] || { echo "  계속하려면 Enter, 중단은 Ctrl-C"; read -r; }
fi

# ── GIT 가드 (2026-08-11 추가) ────────────────────────────────────────
# Pi 의 배포 디렉터리를 git 으로 관리하기 시작하면 rsync 는 그 위에 덮어써서
# 작업트리를 더럽힌다. 그러면 `git rev-parse HEAD` 가 실제 파일과 무관해지고,
# "지금 저 기계에서 뭐가 도나"에 답할 수 없게 된다.
#
# 실제로 그 상태였다: Pi 의 클론이 e2f14db(v16 재구조 **이전** main)에 멈춰
# 있는데 파일은 rsync 로 계속 덮여, git status 가 88항목을 뱉으면서도 그게
# 무슨 의미인지 아무도 알 수 없었다. "팀원 코드를 덮었나"를 확인하는 데만
# 한참 걸렸다 (2026-08-11 git 기반으로 전환).
#
# ⚠ 이 가드를 --force 로 넘기면 그 상태로 되돌아간다. 정말 필요한 경우
#   (git 을 쓸 수 없는 장비 등)가 아니면 Pi 에서 pull 하는 쪽을 쓸 것.
if [ -z "$DRY" ]; then
  GITINFO=$(ssh "$RPI_USER@$RPI_HOST" \
    "cd $DEST 2>/dev/null && git rev-parse --abbrev-ref HEAD --short HEAD 2>/dev/null | tr '\n' ' '" \
    2>/dev/null || true)
  if [ -n "${GITINFO// /}" ]; then
    echo
    echo "⛔ Pi 의 배포 디렉터리가 git 으로 관리된다 — $GITINFO"
    echo "   rsync 로 밀면 작업트리가 더러워지고 HEAD 가 실제 파일과 어긋난다."
    echo
    echo "   배포는 Pi 에서:"
    echo "     cd ~/7th_VEDA_GROUP2 && git pull"
    echo "     cd rpi_b && cmake -B build && cmake --build build -j"
    echo "     sudo systemctl restart guardx-mqttd guardx-poller"
    echo
    echo "   (--dry-run 은 이 가드를 지나간다 — 아무것도 안 바꾸므로)"
    [ "$FORCE" = 1 ] || exit 1
    echo "   ⚠ --force 지정됨 — git 관리 상태를 깨면서 계속한다"
  fi
fi

# ── PREFLIGHT: 남이 쓰고 있으면 재시작하지 않는다 ────────────────────
# 판정 근거 둘.
#  (a) systemd MainPID 가 아닌 guardx_poller/guardx_mqttd 프로세스 = 누군가
#      수동 실행 중. 재시작하면 그 사람 세션이 깨지거나, 같은 client_id 로
#      브로커에 둘이 붙어 서로 끊는다.
#  (b) 나 말고 다른 로그인 세션이 있다 = 누가 붙어서 뭔가 하는 중.
# 둘 중 하나라도 걸리면 **코드만 옮기고 재시작은 건너뛴다**.
OTHERS=""
if [ -z "$DRY" ]; then
  OTHERS=$(ssh "$RPI_USER@$RPI_HOST" bash -s -- "$RPI_USER" <<'PREFLIGHT' || true
me="$1"; found=""
for svc in guardx-poller guardx-mqttd; do
  main=$(systemctl show -p MainPID --value "$svc" 2>/dev/null || echo 0)
  bin="${svc/guardx-/guardx_}"
  for pid in $(pgrep -x "$bin" 2>/dev/null); do
    [ "$pid" = "$main" ] && continue
    owner=$(ps -o user= -p "$pid" | tr -d ' ')
    found="${found}수동실행: $bin pid=$pid user=$owner\n"
  done
done
sessions=$(who 2>/dev/null | awk '{print $1}' | sort -u | grep -v "^${me}$" || true)
[ -n "$sessions" ] && found="${found}다른 로그인: $(echo "$sessions" | tr '\n' ' ')\n"
printf '%b' "$found"
PREFLIGHT
)
fi

if [ -n "$OTHERS" ]; then
  echo
  echo "⚠ 다른 사용자/세션이 붙어 있다 — 재시작을 건너뛴다 (코드만 옮긴다):"
  echo "$OTHERS" | sed 's/^/   /'
  RESTART=0
fi

# ── 동기화 ───────────────────────────────────────────────────────────
rsync -avz --delete $DRY \
  --exclude 'build/' \
  --exclude 'state/' \
  --exclude 'config.env' \
  --exclude 'camera.pem' \
  --exclude 'poller.log' \
  --exclude '.git/' \
  --exclude 'verify_*.json' \
  --exclude '*.bak' --exclude '*.swp' --exclude '.main.cpp.*' \
  "$SRC" "$RPI_USER@$RPI_HOST:$DEST"

[ -n "$DRY" ] && { echo "(dry-run — 여기서 끝)"; exit 0; }
[ "$BUILD" = 1 ] || { echo "✅ 동기화만 완료 (--no-build)"; exit 0; }

# ── 빌드 ─────────────────────────────────────────────────────────────
# 빌드 실패 시 재시작하지 않는다 — 돌던 옛 바이너리가 계속 도는 편이 낫다.
echo
echo "── 빌드 ──"
if ! ssh "$RPI_USER@$RPI_HOST" "cd $DEST && cmake -B build && cmake --build build -j4"; then
  echo "⛔ 빌드 실패 — 재시작하지 않는다 (기존 바이너리 유지)"
  exit 1
fi

if [ "$RESTART" != 1 ]; then
  echo
  echo "✅ 동기화 + 빌드 완료. 재시작은 하지 않았다."
  echo "   직접 하려면: ssh $RPI_USER@$RPI_HOST 'sudo systemctl restart ${SERVICES[*]}'"
  exit 0
fi

# ── 재시작 ───────────────────────────────────────────────────────────
# **Pi 에 등록된 유닛만** 재시작한다. guardx-mqttd 는 브로커와 DB 에만 붙으면
# 되므로 다른 머신(개발 PC)에서 돌리는 구성도 유효하고, 그 경우 Pi 엔 유닛이
# 없다. 없는 유닛을 restart 하면 systemctl 이 실패해 뒤따르는 상태 출력까지
# 통째로 날아간다 — 그래서 존재하는 것만 고른다.
PRESENT=$(ssh "$RPI_USER@$RPI_HOST" bash -s -- "${SERVICES[@]}" <<'UNITS' || true
for s in "$@"; do
  systemctl list-unit-files "$s.service" --no-legend 2>/dev/null | grep -q . && echo "$s"
done
UNITS
)
PRESENT=$(echo "$PRESENT" | tr '\n' ' ' | sed 's/ *$//')

for s in "${SERVICES[@]}"; do
  case " $PRESENT " in
    *" $s "*) ;;
    *) echo "⚠ $s 유닛이 Pi 에 없다 — 건너뛴다."
       echo "   다른 머신에서 돌리는 중이면 정상. Pi 에서 돌리려면:"
       echo "     sudo cp $DEST/systemd/$s.service /etc/systemd/system/"
       echo "     sudo systemctl daemon-reload && sudo systemctl enable --now $s"
       echo "   ⚠ 두 머신에서 동시에 띄우지 말 것 — 같은 retained 토픽을 둘이"
       echo "     발행하면 어느 값이 최종인지 알 수 없고, client_id 가 겹치면"
       echo "     브로커가 서로 끊는다." ;;
  esac
done

if [ -z "$PRESENT" ]; then
  echo "⛔ 재시작할 유닛이 하나도 없다 — 코드·빌드만 완료."
  exit 0
fi

echo
echo "── 재시작: $PRESENT ──"
ssh -t "$RPI_USER@$RPI_HOST" \
  "sudo systemctl restart $PRESENT && sleep 3 && \
   systemctl --no-pager -l status $PRESENT | grep -E 'Loaded:|Active:|●' && \
   echo && echo '── 최근 로그 ──' && \
   sudo journalctl $(for s in $PRESENT; do printf -- '-u %s ' "$s"; done)--since '-20s' --no-pager | tail -15"
