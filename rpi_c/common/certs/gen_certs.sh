#!/bin/bash
# gen_certs.sh - GuardX mTLS용 CA + 노드별 인증서 생성
#
# 실행 위치 무관 (어디서 실행하든 이 스크립트 옆에 out/ 생성).
# 아무 노드에서나 "1회만" 실행해서 out/ 통째로 3개 Pi에 나눠 배포한다.
# !!! 절대 A/B/C 각자에서 따로 실행하지 말 것 - 매번 새 CA가 생겨서
#     서로 인증서 체인이 안 맞게 된다 (벤치마크 디버깅 때 실제로 겪은 문제) !!!
# CA 개인키(ca.key)는 노출되면 전체 mTLS가 뚫리니 배포하지 말고 보관만.
#
# rpib(브로커) 인증서에는 SAN(Subject Alternative Name)을 반드시 넣는다.
# rpia/rpic는 IP(mqtt_pub.h/mqtt_sub.h의 MQTT_BROKER_HOST)로 브로커에
# 접속하는데, libmosquitto는 기본적으로 접속 주소가 인증서의 CN/SAN과
# 일치하는지 검증한다. SAN 없이 CN만 있으면 체인 검증은 통과해도
# 이 단계에서 실패한다 (브로커 로그에 "tlsv1 alert internal error").
#
# 사용법: ./gen_certs.sh [브로커_IP]
#   생략하면 mqtt_pub.h에 하드코딩된 기본값(172.20.33.251)을 사용.
#   브로커 IP가 바뀌면 인자로 새 IP를 넘겨서 재실행할 것.
set -e

RPIB_IP="${1:-172.20.33.251}"

DAYS_CA=3650      # CA 유효기간 10년 (테스트/소규모 프로젝트 기준)
DAYS_NODE=825     # 노드 인증서 유효기간 (공인 CA 관례 상한 근사치)
OUT="$(cd "$(dirname "$0")" && pwd)/out"

mkdir -p "$OUT"
cd "$OUT"

echo "[1/3] CA 생성 (한 번만 실행되면 됨, 이미 있으면 건너뜀)"
if [ ! -f ca.key ]; then
    openssl genrsa -out ca.key 4096
    openssl req -x509 -new -nodes -key ca.key -sha256 -days $DAYS_CA \
        -out ca.crt -subj "/O=GuardX/CN=GuardX-CA"
    echo "  -> ca.key, ca.crt 생성됨"
else
    echo "  -> 기존 CA 재사용 (ca.key, ca.crt)"
fi

echo "[2/3] 노드별 키+인증서 생성 (CN=노드ID, MQTT client id와 일치시킬 것)"
for node in rpia rpib rpic; do
    echo "  -- $node --"
    openssl genrsa -out "${node}.key" 2048

    if [ "$node" = "rpib" ]; then
        # 브로커 인증서만 SAN 필요 (클라이언트가 IP로 접속해서 hostname
        # 검증을 통과해야 함 - 클라이언트 인증서는 검증 방향이 반대라 불필요)
        #
        # SAN을 CSR에 -addext로 넣고 서명 때 -copy_extensions로 옮기는
        # 방식은 OpenSSL 버전을 탄다(3.0 미만엔 그 옵션이 없고, 버전에
        # 따라 조용히 누락되기도 한다 - 실제로 SAN 없는 인증서가 나와서
        # 여기서 잡혔다). 서명 단계에 -extfile로 직접 주는 쪽이 버전과
        # 무관하게 확실하다.
        openssl req -new -key "${node}.key" -out "${node}.csr" \
            -subj "/O=GuardX/CN=${node}"
        cat > "${node}.ext" << EXT
subjectAltName = DNS:rpib, IP:${RPIB_IP}
basicConstraints = CA:FALSE
EXT
        openssl x509 -req -in "${node}.csr" -CA ca.crt -CAkey ca.key \
            -CAcreateserial -out "${node}.crt" -days $DAYS_NODE -sha256 \
            -extfile "${node}.ext"
        rm -f "${node}.ext"

        # 방금 만든 인증서에 SAN이 실제로 들어갔는지 즉시 확인한다.
        # 이게 없으면 배포까지 다 끝낸 뒤 A의 TLS 핸드셰이크에서야
        # 드러나고, 그때는 원인이 인증서인지 네트워크인지 구분이 어렵다.
        if ! openssl x509 -in "${node}.crt" -noout -text \
             | grep -q "IP Address:${RPIB_IP}"; then
            echo "!!! rpib.crt에 SAN(IP:${RPIB_IP})이 없습니다. 중단합니다." >&2
            exit 1
        fi
        echo "     SAN 확인됨: DNS:rpib, IP:${RPIB_IP}"
    else
        openssl req -new -key "${node}.key" -out "${node}.csr" \
            -subj "/O=GuardX/CN=${node}"
        openssl x509 -req -in "${node}.csr" -CA ca.crt -CAkey ca.key \
            -CAcreateserial -out "${node}.crt" -days $DAYS_NODE -sha256
    fi
    rm -f "${node}.csr"
done

echo "[3/3] 완료. 산출물: $OUT"
ls -la "$OUT"

cat << 'EOF'

== 배포 위치 (권장) ==
RPi A: ca.crt, rpia.crt, rpia.key -> /etc/guardx/certs/
RPi B: ca.crt, rpib.crt, rpib.key -> /etc/mosquitto/certs/  (브로커용)
RPi C: ca.crt, rpic.crt, rpic.key -> /etc/guardx/certs/

CA 개인키(ca.key)는 어느 Pi에도 배포하지 말 것 - 이후 인증서 추가
발급이 필요할 때만 이 스크립트를 돌린 기기에 보관.

권한 설정 예시 (각 Pi에서):
  sudo mkdir -p /etc/guardx/certs
  sudo cp ca.crt <node>.crt <node>.key /etc/guardx/certs/
  sudo chmod 600 /etc/guardx/certs/<node>.key
  sudo chown root:root /etc/guardx/certs/*
EOF
