#!/usr/bin/env python3
"""
modbus_watch.py - RPi C가 STM32로 내보내는 Modbus 쓰기 프레임 모니터 (읽기 전용)

STM32 실물을 붙이기 전에 "rpic_subscriber가 MQTT를 받아 실제로 올바른
레지스터에 올바른 값을 쓰는가"를 눈으로 확인하는 도구다. 응답은 하지 않는다 -
가짜 슬레이브를 흉내내면 그쪽 구현이 또 하나의 검증 대상이 되어버려서,
여기서는 "나가는 바이트"만 해석해 보여준다.

  ┌ rpic_subscriber ─┐        ┌─ 이 스크립트 ─┐
  │ GUARDX_MATRIX_DEV│──pty──▶│ 프레임 해석    │
  └──────────────────┘        └───────────────┘

test_matrix_link.c와 역할이 다르다. 저쪽은 matrix_link.c 함수를 직접 불러
변환 로직만 보고(빠르고 자동), 이쪽은 MQTT 구독·라우팅·워커 스레드·termios
까지 실제 바이너리를 그대로 통과시킨다(느리지만 통합 경로 전체).

사용법
------
  # 터미널 1 - 가짜 시리얼 한 쌍을 만들고 STM32 쪽 끝을 감시
  socat -d -d pty,raw,echo=0,link=/tmp/fake_stm32 pty,raw,echo=0,link=/tmp/fake_pi &
  python3 modbus_watch.py /tmp/fake_stm32

  # 터미널 2 - 데몬을 진짜 시리얼 대신 가짜 쪽에 물린다
  cd ../app && GUARDX_MATRIX_DEV=/tmp/fake_pi ./rpic_subscriber

  # 터미널 3 - 찔러보기
  mosquitto_pub -h <브로커> -t guardx/display/rpic/fire -q 1 -r -m '{"zone_bitmap":5}'
  mosquitto_pub -h <브로커> -t guardx/display/rpic/zones/1 -q 1 -r \
      -m '{"zone_id":1,"temp_x10":235,"humidity":45}'

기대 출력
---------
  [10:20:31] FC06  FIRE_ZONE_BITMAP(120) = 5   CRC OK
  [10:20:44] FC10  ZONE1_TEMP_X10=235, ZONE1_HUM=45   CRC OK

응답해줄 STM32가 없으므로 같은 프레임이 3번씩(최초 + 재시도 2회) 찍히고
rpic_subscriber 쪽엔 타임아웃 로그가 난다 - 정상이다.
"""
import sys
import time

PORT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/fake_stm32"

# guardx_modbus_regs.h의 PDU 주소와 같아야 한다. 이름이 틀리면 값이 맞아도
# 사람이 "엉뚱한 레지스터에 썼다"고 오판한다.
NAMES = {
    100: "ZONE1_TEMP_X10", 101: "ZONE1_HUM",
    102: "ZONE2_TEMP_X10", 103: "ZONE2_HUM",
    104: "ZONE3_TEMP_X10", 105: "ZONE3_HUM",
    106: "ZONE4_TEMP_X10", 107: "ZONE4_HUM",
    120: "FIRE_ZONE_BITMAP",
    121: "TRACK_STATUS",
    122: "CUR_X", 123: "CUR_Y",
    124: "DIR_X", 125: "DIR_Y",
}


def crc16(data):
    """Modbus CRC-16 (poly 0xA001, init 0xFFFF). 하위바이트가 먼저 전송된다."""
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def reg_name(addr):
    return NAMES.get(addr, f"REG{addr}")


def frame_len(buf):
    """완성에 필요한 총 길이. 더 받아야 하면 None, 모르는 FC면 -1.

    RTU는 길이 필드가 없고 무수신 구간으로 프레임을 나누지만, 우리가 보는 것은
    마스터의 쓰기 요청 두 종류뿐이라 FC로 길이가 결정된다 - 타이밍에 기대지
    않아도 되므로 스케줄링 지터에 흔들리지 않는다.
    """
    if len(buf) < 2:
        return None
    fc = buf[1]
    if fc == 0x06:                       # slave+fc+addr(2)+value(2)+crc(2)
        return 8
    if fc == 0x10:                       # ...+qty(2)+bytecount(1)+data+crc(2)
        if len(buf) < 7:
            return None
        return 9 + buf[6]
    return -1


def main():
    buf = bytearray()
    print(f"모니터 시작: {PORT}   (Ctrl+C 종료)")

    with open(PORT, "rb", buffering=0) as port:
        while True:
            chunk = port.read(1)
            if not chunk:
                time.sleep(0.01)         # 아직 쓰는 쪽이 없다
                continue

            buf += chunk
            need = frame_len(buf)
            if need is None:
                continue
            if need < 0:
                print(f"  ? 알 수 없는 FC 0x{buf[1]:02X} - 버림  {buf.hex(' ')}")
                buf.clear()
                continue
            if len(buf) < need:
                continue

            frame = bytes(buf[:need])
            del buf[:need]

            ok = "OK" if crc16(frame[:-2]) == (frame[-2] | (frame[-1] << 8)) \
                 else "**오류**"
            ts = time.strftime("%H:%M:%S")
            addr = (frame[2] << 8) | frame[3]

            if frame[1] == 0x06:
                value = (frame[4] << 8) | frame[5]
                print(f"[{ts}] FC06  {reg_name(addr)}({addr}) = {value}   CRC {ok}")
            else:
                qty = (frame[4] << 8) | frame[5]
                vals = [(frame[7 + 2 * i] << 8) | frame[8 + 2 * i]
                        for i in range(qty)]
                body = ", ".join(f"{reg_name(addr + i)}={v}"
                                 for i, v in enumerate(vals))
                print(f"[{ts}] FC10  {body}   CRC {ok}")

            print(f"         raw: {frame.hex(' ')}")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print()
