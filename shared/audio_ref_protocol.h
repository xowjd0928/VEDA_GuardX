#ifndef GUARDX_AUDIO_REF_PROTOCOL_H
#define GUARDX_AUDIO_REF_PROTOCOL_H

/*
 * GuardX speaker reference stream — RPi C internal.
 *
 * TOIMIC(비명·총성 감지)은 방송이나 사이렌이 나가는 동안에도 감지를 계속
 * 해야 한다. 그러려면 마이크가 되받은 스피커 소리를 지워야 하고, 지우려면
 * "스피커로 무엇이 나갔는가"를 알아야 한다 - 그게 이 스트림이다.
 *
 * 예전에는 재생 중에 감지를 통째로 껐다(detector.py 의 speaker_running).
 * 안전한 선택이었지만, 방송 중에 난 비명과 사이렌 중에 난 총성은 영영 못
 * 잡는다. 하필 그때가 제일 중요한 순간이다.
 *
 * ── 왜 마이크로 되잡지 않고 디지털 사본을 보내는가 ──
 * 재생과 캡처를 ALSA 루프백으로 엮는 방법도 있지만, 그러면 재생 경로 전체가
 * 루프백 장치와 전달 프로세스에 얹혀 방송·사이렌의 실패 지점이 늘어난다.
 * 안전 기능(사이렌)이 감지 기능 때문에 더 잘 죽는 구조는 방향이 거꾸로다.
 * 그래서 소리를 내는 쪽이 자기가 낸 것을 그대로 한 부 더 보낸다 - 그 경로가
 * 끊겨도 재생은 아무 영향이 없다.
 *
 * ── 발신자 ──
 *   사이렌·상황음 : app/src/rpic_audio.c (ALSA 로 쓰기 직전에 tap)
 *   방송          : broadcast_rtp/receive.sh (GStreamer tee 분기)
 * 둘은 조율기가 배타 점유를 강제하므로 동시에 보내지 않는다.
 *
 * ── 형식 ──
 * 헤더 없는 raw PCM: S16LE, 모노, 16 kHz. 감지기가 AEC 를 도는 레이트에
 * 맞춰 두어 수신 측 변환을 없앴다 - 어긋나면 조용히 성능만 나빠지는 종류의
 * 버그라, 형식을 한 곳에 고정한다.
 *
 * 데이터그램 하나 = 20 ms = 320 샘플 = 640 바이트. MTU 안에 들어가고,
 * 잃어버려도 그 20 ms 만큼만 AEC 가 어긋난다(적응 필터가 곧 따라잡는다).
 *
 * 유실을 걱정하지 않는 이유: 기준 신호는 명령이 아니라 참고 자료다.
 * TCP 로 만들면 재생 스레드가 수신자 사정에 묶인다 - 스피커가 감지기 때문에
 * 끊기는 것이 훨씬 나쁘다.
 */

#define GUARDX_AUDIO_REF_HOST      "127.0.0.1"
#define GUARDX_AUDIO_REF_PORT      5005
#define GUARDX_AUDIO_REF_RATE      16000
#define GUARDX_AUDIO_REF_CHANNELS  1
#define GUARDX_AUDIO_REF_FRAME_MS  20
#define GUARDX_AUDIO_REF_SAMPLES   320   /* 20 ms @ 16 kHz */
#define GUARDX_AUDIO_REF_BYTES     (GUARDX_AUDIO_REF_SAMPLES * 2)

#endif /* GUARDX_AUDIO_REF_PROTOCOL_H */
