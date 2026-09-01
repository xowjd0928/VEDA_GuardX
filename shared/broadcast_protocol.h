#ifndef GUARDX_BROADCAST_PROTOCOL_H
#define GUARDX_BROADCAST_PROTOCOL_H

/*
 * GuardX basic live-broadcast protocol.
 *
 * Command messages are JSON. Stream messages are a compact binary header
 * followed by fixed-format PCM so the basic version needs no codec or format
 * negotiation layer.
 */

#define GUARDX_BROADCAST_COMMAND_TOPIC "guardx/broadcast/rpic/command"
#define GUARDX_BROADCAST_STREAM_TOPIC  "guardx/broadcast/rpic/stream"

/*
 * RPi C -> VMS. 수신기가 실제로 떠서 스피커를 잡았다는 응답.
 *   {"node_id":"rpic","session_id":N,"result":"ready"|"error","reason":"..."}
 *
 * 이게 필요한 이유: 수신기는 방송 중에만 뜬다(평상시 스피커를 비워 화재
 * 사이렌이 언제든 장치를 열 수 있게 하려고). 기동에 시간이 걸리므로 VMS 가
 * START 직후 바로 쏘면 앞부분이 잘린다.
 */
#define GUARDX_BROADCAST_READY_TOPIC   "guardx/broadcast/rpic/ready"

/*
 * RPi C -> 모든 VMS. 지금 누가 스피커를 쥐고 있는가. **retained** 다.
 *   {"node_id":"rpic","timestamp":N,"active":true,"session_id":N,
 *    "owner":"vms-호스트명","reason":""}
 *
 * VMS 가 여러 대일 때 "다른 VMS 가 방송 중인가"를 물어볼 곳이 필요하다.
 * 그 답을 아는 것은 스피커를 실제로 쥔 RPi C 뿐이다 - VMS 끼리 서로 알린다면
 * 죽은 VMS 의 마지막 주장이 영원히 남는다. retained 라서 나중에 켜진 VMS 도
 * 구독 즉시 현재 소유자를 받는다.
 *
 * reason 은 active=false 로 바뀐 사유다. 방송이 끊긴 VMS 가 화면에 무슨 일이
 * 있었는지 적어야 하므로, "꺼졌다"만으로는 부족하다:
 *   ""           방송 없음(기동 직후)
 *   "stopped"    운영자가 껐다
 *   "taken_over" 다른 VMS 가 방송권을 가져갔다
 *   "timeout"    KEEPALIVE 가 끊겼다 (VMS 종료·네트워크 단절)
 *   "fire"       화재가 방송을 선점했다
 *   "error"      수신기 기동 실패
 */
#define GUARDX_BROADCAST_STATE_TOPIC   "guardx/broadcast/rpic/state"

#define GUARDX_BROADCAST_ACTION_START     "START"
#define GUARDX_BROADCAST_ACTION_STOP      "STOP"
/* 방송 중 주기 재발행. VMS 가 비정상 종료해 STOP 이 유실돼도 RPi C 가
 * 스스로 방송을 접고 장치를 반납하게 만든다. payload 는 START 와 같다. */
#define GUARDX_BROADCAST_ACTION_KEEPALIVE "KEEPALIVE"

/*
 * START 에 실리는 추가 필드 (다중 VMS).
 *
 *   "owner"    : 발신 VMS 의 MQTT client id. 화면에 "누가 방송 중인지"를
 *                보여주기 위한 것이고, 세션 판별은 여전히 session_id 로 한다.
 *   "takeover" : true 면 진행 중인 다른 세션을 끊고 가져간다. 없거나 false 면
 *                이미 다른 세션이 있을 때 RPi C 가 거절한다("busy").
 *
 * 기본이 거절인 이유: 예전에는 나중 START 가 무조건 이겼다. VMS 두 대가 서로
 * 모른 채 방송을 걸면 앞사람의 방송이 예고 없이 끊긴다. 인수는 운영자가
 * 확인창에서 명시적으로 고른 경우에만 일어나야 한다.
 */
#define GUARDX_BROADCAST_FIELD_OWNER    "owner"
#define GUARDX_BROADCAST_FIELD_TAKEOVER "takeover"

/* READY 응답의 result 값. "busy" 는 다른 세션이 쥐고 있다는 뜻이고, 그때
 * owner 필드에 현재 소유자가 실린다 - VMS 는 그걸 확인창에 보여준다. */
#define GUARDX_BROADCAST_RESULT_READY "ready"
#define GUARDX_BROADCAST_RESULT_ERROR "error"
#define GUARDX_BROADCAST_RESULT_BUSY  "busy"

/* VMS 재발행 주기 / RPi C 만료 판정 / VMS 의 READY 대기 상한 (ms). */
#define GUARDX_BROADCAST_KEEPALIVE_MS  2000
#define GUARDX_BROADCAST_TIMEOUT_MS    5000
#define GUARDX_BROADCAST_READY_WAIT_MS 4000

#define GUARDX_BROADCAST_SAMPLE_RATE       48000U
#define GUARDX_BROADCAST_CHANNELS          2U
#define GUARDX_BROADCAST_SAMPLE_BYTES      2U
#define GUARDX_BROADCAST_FRAME_MS          20U
#define GUARDX_BROADCAST_SAMPLES_PER_FRAME 960U
#define GUARDX_BROADCAST_PCM_BYTES         3840U

/*
 * Binary stream packet (all multibyte integers are little-endian):
 *   0..3   magic "GXAU"
 *   4      protocol version (1)
 *   5      channels (2)
 *   6..7   samples per channel (960)
 *   8..11  broadcast session id
 *   12..15 packet sequence number
 *   16..   interleaved PCM S16_LE (L, R), exactly 3840 bytes
 */
#define GUARDX_BROADCAST_MAGIC_0       'G'
#define GUARDX_BROADCAST_MAGIC_1       'X'
#define GUARDX_BROADCAST_MAGIC_2       'A'
#define GUARDX_BROADCAST_MAGIC_3       'U'
#define GUARDX_BROADCAST_VERSION       1U
#define GUARDX_BROADCAST_HEADER_BYTES  16U
#define GUARDX_BROADCAST_PACKET_BYTES  \
    (GUARDX_BROADCAST_HEADER_BYTES + GUARDX_BROADCAST_PCM_BYTES)

#define GUARDX_BROADCAST_QOS_COMMAND 1
#define GUARDX_BROADCAST_QOS_STREAM  0

/*
 * --- 업그레이드 경로: Opus/RTP/UDP 미디어 전송 (기존 정의는 위에서 불변) ---
 *
 * 미디어를 MQTT PCM 스트림에서 RTP-over-UDP + Opus로 옮기는 옵션 경로의 상수.
 * 위의 PCM/MQTT 경로와 병존하며 기본값이 아니다(VMS 레지스트리 broadcast/transport
 * 로 선택). RPi C 수신부는 별도 GStreamer 프로세스(broadcast_rtp/)라 이 상수만
 * 양쪽이 맞추면 된다. Opus RTP clock-rate는 규격상 항상 48000이다.
 */
/*
 * 방송을 보낼 목적지 = RPi C. 여기서 수신기(broadcast_rtp/receive.sh)가 돈다.
 *
 * ⚠ MQTT 브로커 주소(Credentials::mqtt_host)를 대신 쓰지 말 것.
 *   - 브로커      = RPi B, 172.20.33.251
 *   - 방송 목적지 = RPi C, 172.20.33.114   ← 이 상수
 *   서로 다른 기계다.
 *
 * 예전에 이 상수가 없어서 목적지를 브로커 주소로 채웠고, 그래서 방송이 RPi B로
 * 날아갔다. RPi B에는 UDP 5004를 듣는 프로그램이 없어 패킷은 그냥 버려진다.
 * UDP는 응답이 없으므로 VMS는 그걸 알 방법이 없어 "방송 중"을 계속 표시했다.
 * 목적지 값이 이미 저장돼 있던 PC 한 대에서만 우연히 동작해 한동안 안 드러났다.
 */
#define GUARDX_BROADCAST_RTP_HOST     "172.20.33.114"
#define GUARDX_BROADCAST_RTP_PORT     5004
#define GUARDX_BROADCAST_RTP_PAYLOAD  96
/*
 * 64 kbps 모노 = 안내방송용으로 충분히 투명한 구간. 24k는 대역폭이 아니라
 * 명료도에서 손해가 컸다(자음이 뭉갠다). LAN 전제라 8 kB/s 차이는 무의미.
 * 현장이 저대역폭이면 VMS 레지스트리 broadcast/opus_bitrate 로 낮춘다.
 */
#define GUARDX_BROADCAST_OPUS_BITRATE 64000

#endif /* GUARDX_BROADCAST_PROTOCOL_H */
