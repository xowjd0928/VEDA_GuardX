#include "Poller/task_alert.hpp"
#include "Mqtt/mqtt_pub.hpp"
#include "Poller/task_fan_level.hpp"
#include <iostream>
#include <optional>
#include <string>

// 상태 전이 규칙 (hysteresis):
//   none -> warn/critical : incidents INSERT + alerts INSERT
//   warn -> critical      : incidents severity UPDATE + alerts INSERT (승급)
//   critical -> warn      : 발화 없음 — open 유지 (강등 플랩 방지)
//   open -> clear         : 채택 비율이 warn_ratio*0.9 미만이면 resolved
//                           (0.9 = 해제 히스테리시스 밴드 — 경계 플랩 방지)
// 신선도: 예측은 3분(60초 틱 2회 유예), 실측은 5분. 둘 다 stale이면 판단
// 불가 → 아무것도 안 함 (개장 전·폴러 재시작 직후 오탐 방지).
//
// source_type: 예측 우세면 'prediction'(source_id=prediction_id),
// 실측 우세면 'detection'(zone_occupancy는 detections 파생, 행 id 없음 → NULL).
//
// 읽기만 하고 끝나는 경로는 commit 없이 return — pqxx::work 소멸자가 abort
// 하며, commit()은 "이 경로는 썼다"의 표지로만 남긴다.

namespace {

int sevOf(double ratio, double warn, double crit) {
  if (ratio >= crit) return 2;
  if (ratio >= warn) return 1;
  return 0;
}

const char* sevName(int sev) { return sev == 2 ? "critical" : "warn"; }

// "zone_A(ch1) 예측 8/10명 (80%)" — 경보 메시지 공통 꼬리
std::string describe(const std::string& zname, int channel,
                     const std::string& source_type, int count, int cap) {
  return zname + "(ch" + std::to_string(channel) + ") " +
         (source_type == "prediction" ? "예측 " : "실측 ") +
         std::to_string(count) + "/" + std::to_string(cap) + "명 (" +
         std::to_string((int)((double)count / cap * 100.0 + 0.5)) + "%)";
}

void insertAlert(pqxx::work& tx, int iid, const std::string& msg) {
  tx.exec(
      "INSERT INTO alerts (incident_id, message, broadcast_channel)"
      " VALUES ($1, $2, 'vms_popup')",
      pqxx::params{iid, msg});
}

}  // namespace

void pollAlert(pqxx::connection& db, int zone_id, int channel,
               int current_count) {
  try {
    pqxx::work tx(db);

    // 임계 로드 (존 이름 포함 — 경보 메시지용)
    pqxx::result th = tx.exec(
        "SELECT t.capacity_limit, t.warn_ratio, t.critical_ratio, z.zone_name"
        " FROM zone_thresholds t JOIN zones z USING (zone_id)"
        " WHERE t.zone_id = $1 AND t.capacity_limit IS NOT NULL",
        pqxx::params{zone_id});
    if (th.empty()) {
      reportZoneLevel(zone_id, -1);   // 팬: 모름 (정상 0 과 구분)
      return;   // 임계 미설정 존 — 판단 불가 (정상)
    }
    const int    cap  = th[0][0].as<int>();
    const double warn = th[0][1].as<double>();
    const double crit = th[0][2].as<double>();
    const std::string zname = th[0][3].as<std::string>();
    if (cap <= 0) {
      reportZoneLevel(zone_id, -1);
      return;
    }

    // 최신 예측 배치의 최근접 horizon (predicted_at 동률 4행 중 target 최소).
    // 피드백 4호: warmup 행은 경보 판단에서 제외 (NULL = 구 적재분 = 비-warmup).
    pqxx::result pr = tx.exec(
        "SELECT prediction_id, predicted_count FROM congestion_prediction"
        " WHERE zone_id = $1 AND predicted_at > now() - interval '3 minutes'"
        "   AND warmup IS NOT TRUE"
        " ORDER BY predicted_at DESC, target_ts ASC LIMIT 1",
        pqxx::params{zone_id});

    // 최신 실측 (카메라가 0도 backfill — 행 자체가 없으면 미관측)
    pqxx::result oc = tx.exec(
        "SELECT person_count FROM zone_occupancy"
        " WHERE zone_id = $1 AND bucket_ts > now() - interval '5 minutes'"
        " ORDER BY bucket_ts DESC LIMIT 1",
        pqxx::params{zone_id});

    if (pr.empty() && oc.empty() && current_count < 0) {
      reportZoneLevel(zone_id, -1);
      return;   // 전 신호 불명
    }

    // 3신호(현재 인원·분 중앙값·예측) 중 비율 나쁜 쪽 채택. sevOf는 ratio에
    // 단조라 비교는 ratio로 충분 (동률이면 예측 우선 — source_id 채울 수 있는 쪽).
    // 현재값 덕에 급증은 이번 틱에 잡히고, 순간 드롭아웃(현재 0)은 중앙값이
    // 바닥을 받쳐 오해제(false clear)가 안 난다.
    int count = 0;
    double ratio = -1.0;
    std::string source_type = "detection";
    std::optional<long long> source_id;
    if (current_count >= 0) {
      count = current_count;
      ratio = (double)current_count / cap;
    }
    if (!oc.empty()) {
      const int mc = oc[0][0].as<int>();
      if ((double)mc / cap > ratio) { count = mc; ratio = (double)mc / cap; }
    }
    if (!pr.empty()) {
      const int pc = pr[0][1].as<int>();
      const double pratio = (double)pc / cap;
      if (pratio >= ratio) {
        count = pc; ratio = pratio;
        source_type = "prediction";
        source_id   = pr[0][0].as<long long>();
      }
    }
    const int sev = sevOf(ratio, warn, crit);

    // ⚠ 팬 단계로 sev(순간값)를 쓰면 안 된다. 아래 상태머신은 히스테리시스가
    // 걸려 있어 critical -> warn 강등을 반영하지 않고(플랩 방지), 해제도
    // warn*0.9 밴드를 내려가야 한다. sev 를 그대로 쓰면 VMS 화면은 critical
    // 인데 팬은 75%로 내려가 있는, 서로 다른 두 단계가 생긴다.
    // 그래서 보고는 각 전이가 확정된 자리에서 한다.

    // open incident 조회 (zone당 최대 1건 유지가 규약)
    pqxx::result open = tx.exec(
        "SELECT incident_id, severity FROM incidents"
        " WHERE zone_id = $1 AND incident_type = 'congestion'"
        "   AND status = 'open'"
        " ORDER BY detected_at DESC LIMIT 1",
        pqxx::params{zone_id});

    if (open.empty()) {
      if (sev == 0) {
        // 경보가 없다 = VMS 화면도 정상. 같은 뜻의 문자열로 넘긴다.
        reportZoneLevel(zone_id, fanLevelFromSeverityName("clear"));
        return;   // 평상시 — 최빈 경로
      }
      // none -> warn/critical
      pqxx::result ins = tx.exec(
          "INSERT INTO incidents"
          " (zone_id, incident_type, source_type, source_id, severity)"
          " VALUES ($1, 'congestion', $2, $3, $4) RETURNING incident_id",
          pqxx::params{zone_id, source_type, source_id, sevName(sev)});
      const int iid = ins[0][0].as<int>();
      const std::string msg = "[혼잡 " + std::string(sevName(sev)) + "] " +
          describe(zname, channel, source_type, count, cap);
      insertAlert(tx, iid, msg);
      tx.commit();
      // 아래 mqttPublishAlert 에 넘기는 것과 **같은 문자열**을 쓴다.
      reportZoneLevel(zone_id, fanLevelFromSeverityName(sevName(sev)));
      std::cout << "[alert] zone " << zone_id << " OPEN " << sevName(sev)
                << " (" << msg << ")\n";
      // 상황 통지 (커밋 후 — DB가 진실원천, 전이 확정 시에만). 액추에이터
      // 결정은 transmission layer 소관 — 여기는 상황만 알린다.
      mqttPublishAlert(zone_id, channel, iid, sevName(sev), count, cap,
                       source_type.c_str());
      return;
    }

    const int iid = open[0][0].as<int>();

    if (sev == 2 && open[0][1].as<std::string>() == "warn") {
      // warn -> critical 승급
      tx.exec("UPDATE incidents SET severity = 'critical' WHERE incident_id = $1",
              pqxx::params{iid});
      insertAlert(tx, iid, "[혼잡 critical 승급] " +
                               describe(zname, channel, source_type, count, cap));
      tx.commit();
      reportZoneLevel(zone_id, fanLevelFromSeverityName("critical"));
      std::cout << "[alert] zone " << zone_id << " ESCALATE critical\n";
      mqttPublishAlert(zone_id, channel, iid, "critical", count, cap,
                       source_type.c_str());
      return;
    }

    // 해제 판정: 채택값(나쁜 쪽)이 해제 밴드 미만 — 예측·실측 모두 낮다는 뜻
    if (ratio < warn * 0.9) {
      tx.exec("UPDATE incidents SET status = 'resolved' WHERE incident_id = $1",
              pqxx::params{iid});
      tx.commit();
      reportZoneLevel(zone_id, fanLevelFromSeverityName("clear"));
      std::cout << "[alert] zone " << zone_id << " RESOLVED (ratio "
                << (int)(ratio * 100.0 + 0.5) << "%)\n";
      mqttPublishAlert(zone_id, channel, iid, "clear", count, cap,
                       source_type.c_str());
      return;
    }

    // open 유지 (동일 단계·해제 밴드 내 강등) — 발화 없음, 읽기 전용.
    // 팬은 **경보에 남아 있는 단계**를 따른다. 여기가 강등이 무시되는
    // 자리이므로, sev 가 아니라 open incident 의 severity 를 써야
    // VMS 화면과 팬이 같은 단계가 된다.
    reportZoneLevel(zone_id,
                    fanLevelFromSeverityName(open[0][1].as<std::string>()));
  } catch (const std::exception& e) {
    std::cerr << "[alert] db error: " << e.what() << "\n";
  }
}
