#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QVector>

class QNetworkAccessManager;
class QNetworkReply;
class QUrl;

/**
 * @brief 기동 시 카메라 인코더를 저지연 설정으로 강제하는 SUNAPI 클라이언트
 *
 * 사람 손 설정에 의존하지 않고 VMS가 뜰 때마다 멱등하게 밀어 넣는다.
 * (SUNAPI_video_audio_2.6.6 문서 기준 — media.cgi videoprofile/wisestream)
 *
 *  - GOVLength = 1×FrameRate  : 카메라 기본값 8×FrameRate가 지연의 주범
 *  - BitrateControlType = CBR : VBR 스파이크로 인한 송신 버퍼 대기 방지
 *  - DynamicGOV / SmartCodec / DynamicFPS = 끔 : 정적 장면에서 GOP/fps를
 *    늘리는 절약 기능들 — 전부 지연을 키운다
 *  - EntropyCoding = CAVLC    : 디코드가 약간 빠름 (실패 무방 — 별도 요청)
 *  - WiseStream = Off         : 동적 GOP 확장 절약 모드, 저지연과 상극
 */
class CameraTuner : public QObject
{
    Q_OBJECT

public:
    /**
     * @param channels 튜닝할 센서 채널 목록 (0..3)
     * @param profiles 튜닝할 videoprofile 번호 목록 (RTSP가 실제 쓰는 것만)
     *
     * 접속 정보는 Credentials에서 가져온다 (소스에 비밀번호를 두지 않음).
     */
    CameraTuner(const QVector<int> &channels, const QVector<int> &profiles,
                QObject *parent = nullptr);

    /** @brief 현재 설정 조회 -> 저지연 값으로 set 요청 발사 */
    void start();

    /**
     * @brief I-frame 즉시 방출 요청 (2026-08-04)
     *
     * 프로파일 전환(그리드↔풀스크린)의 검은 화면은 대부분 첫 키프레임
     * 대기다 — GOV=60이면 평균 1초, 최악 2초. 새 스트림 재생 직후 이걸
     * 쏘면 카메라가 IDR을 바로 뱉어 전환이 ~0.2초대로 준다.
     * (SUNAPI media.cgi setsynchronizationpoint, guest 권한)
     */
    void request_sync_point(int channel, int profile);

private:
    QUrl cgi(const QString &query) const;
    void on_profiles(QNetworkReply *reply);
    void on_wisestream(QNetworkReply *reply);
    void build_queue();
    void send_next();

    QVector<int> m_channels;
    QVector<int> m_profiles;

    QNetworkAccessManager *m_net = nullptr;
    QStringList m_queue; ///< 남은 update 요청 쿼리들 (순차 전송)

    /// 현재 상태 — 이미 원하는 값이면 쓰지 않기 위해 먼저 읽는다
    QHash<QString, QString> m_state;
    QSet<int> m_wisestream_on;   ///< WiseStream이 Off가 아닌 채널
};
