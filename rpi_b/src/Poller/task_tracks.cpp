#include "Poller/task_tracks.hpp"

#include "Database/db.hpp"
#include "Poller/http_client.hpp"

#include <nlohmann/json.hpp>

#include <iostream>
#include <optional>
#include <string>
#include <stdexcept>
#include <utility>

using json = nlohmann::json;

namespace {

constexpr int DEFAULT_PERSON_CATEGORY = 1;

template <typename T>
std::optional<T> get_optional_value(const json& object, const char* key)
{
    const auto iter = object.find(key);
    if (iter == object.end() || iter->is_null()) {
        return std::nullopt;
    }

    return iter->get<T>();
}

/**
 * @brief optional -> json (없으면 null)
 *
 * nlohmann/json 3.11.3(Pi 의 시스템 패키지)에는 std::optional 직렬화가 없다.
 * `attributes["k"] = std::optional<double>{}` 는 컴파일이 안 된다 —
 * "no match for operator=". 값이 없을 때 키를 빼지 않고 **null 로 남기는**
 * 원래 의도를 유지하려면 이렇게 명시적으로 변환해야 한다.
 */
template <typename T>
json optional_to_json(const std::optional<T>& value)
{
    if (!value) {
        return json(nullptr);
    }

    return json(*value);
}

template <typename T>
T get_value_or_default(const json& object, const char* key, T default_value)
{
    const auto iter = object.find(key);
    if (iter == object.end() || iter->is_null()) {
        return default_value;
    }

    return iter->get<T>();
}

/**
 * 신원 해석 — display_id 가 없으면 global_id 로 대체한다.
 *
 * 이 규칙이 호출부마다 갈리면 안 된다. 예전엔 함수마다 각자
 * get_value_or_default(track, "display_id", 0) 을 불렀는데, 페이로드에
 * display_id 가 없으면 전부 0 이 되어 `ON CONFLICT (display_id)` 가 **서로 다른
 * 사람을 display_id=0 한 행으로 뭉갠다**. VMS DetectionFeed 도 같은 대체 규칙을
 * 쓴다 (display_id ?? global_id) — 양쪽이 같은 사람을 같은 id 로 봐야 한다.
 */
long long resolve_display_id(const json& track)
{
    const long long display_id = get_value_or_default<long long>(track, "display_id", 0);
    if (display_id > 0) {
        return display_id;
    }

    return get_value_or_default<long long>(track, "global_id", 0);
}

/** @brief 카메라를 넘어 같은 사람을 묶는 값. 없으면 display_id 로 대체 */
long long resolve_global_id(const json& track)
{
    const long long global_id = get_value_or_default<long long>(track, "global_id", 0);
    if (global_id > 0) {
        return global_id;
    }

    return resolve_display_id(track);
}

int get_raw_channel(const json& track)
{
    if (track.contains("raw_channel") && !track["raw_channel"].is_null()) {
        return track["raw_channel"].get<int>();
    }

    if (track.contains("channel") && !track["channel"].is_null()) {
        return track["channel"].get<int>() - 1;
    }

    return 0;
}

int get_view_channel(const json& track)
{
    if (track.contains("channel") && !track["channel"].is_null()) {
        return track["channel"].get<int>();
    }

    return get_raw_channel(track) + 1;
}

std::string make_track_attributes(const json& track)
{
    json attributes;

    attributes["global_id"] = resolve_global_id(track);
    attributes["display_id"] = resolve_display_id(track);
    attributes["object_id"] = get_value_or_default<int>(track, "object_id", 0);
    attributes["raw_channel"] = get_raw_channel(track);
    attributes["channel"] = get_view_channel(track);
    attributes["direction"] = get_value_or_default<std::string>(track, "direction", "UNKNOWN");
    attributes["speed"] = get_value_or_default<double>(track, "speed", 0.0);
    attributes["velocity_x"] = get_value_or_default<double>(track, "velocity_x", 0.0);
    attributes["velocity_y"] = get_value_or_default<double>(track, "velocity_y", 0.0);
    attributes["predicted_x"] = optional_to_json(get_optional_value<double>(track, "predicted_x"));
    attributes["predicted_y"] = optional_to_json(get_optional_value<double>(track, "predicted_y"));
    attributes["next_channel_hint"] = get_value_or_default<int>(track, "next_channel_hint", -1);
    attributes["raw_next_channel_hint"] = get_value_or_default<int>(track, "raw_next_channel_hint", -1);
    attributes["handover_ready"] = get_value_or_default<bool>(track, "handover_ready", false);
    attributes["prediction_confidence"] =
        get_value_or_default<double>(track, "prediction_confidence", 0.0);

    return attributes.dump();
}

long long upsert_track(pqxx::work& tx,
                       const Config& cfg,
                       const json& track,
                       const std::string& timestamp)
{
    const long long display_id = resolve_display_id(track);
    const long long global_id = resolve_global_id(track);
    // WiseAI 가 아직 안 실어 보낼 수 있다 — 오면 그대로 저장되고, 없으면 0.
    // 신원 판정에는 쓰지 않는다 (아래 pollTracks 주석 참조).
    const int object_id = get_value_or_default<int>(track, "object_id", 0);
    const int view_channel = get_view_channel(track);
    const std::string state = get_value_or_default<std::string>(track, "state", "active");
    const std::string attributes = make_track_attributes(track);

    const pqxx::result result = tx.exec(
    "INSERT INTO tracks"
    " (display_id, global_id, camera_id, channel, object_id, last_object_id,"
    "  first_seen_at, last_seen_at, state, attributes)"
    " VALUES ($1, $2, $3, $4, $5, $6, $7, $7, $8, $9::jsonb)"
    " ON CONFLICT (display_id)"
    " DO UPDATE SET"
    "   global_id = EXCLUDED.global_id,"
    "   camera_id = EXCLUDED.camera_id,"
    "   channel = EXCLUDED.channel,"
    "   object_id = EXCLUDED.object_id,"
    "   last_object_id = EXCLUDED.last_object_id,"
    "   last_seen_at = EXCLUDED.last_seen_at,"
    "   state = EXCLUDED.state,"
    "   attributes = EXCLUDED.attributes"
    " RETURNING track_id",
    pqxx::params{
        display_id,
        global_id,
        cfg.camera_id,
        view_channel,
        object_id,
        object_id,
        timestamp,
        state,
        attributes
    });

    if (result.empty()) {
        throw std::runtime_error("track upsert failed: empty result");
    }

    return result[0][0].as<long long>();
}

void insert_detection(pqxx::work& tx,
                      const Config& cfg,
                      const json& track,
                      const std::string& timestamp)
{
    const int raw_channel = get_raw_channel(track);
    const int view_channel = get_view_channel(track);
    const long long display_id = resolve_display_id(track);
    const long long global_id = resolve_global_id(track);

    tx.exec(
        "INSERT INTO detections"
        " (camera_id, channel, raw_channel, object_id, display_id, global_id,"
        "  category, likelihood, rect_sx, rect_sy, rect_ex, rect_ey,"
        "  geom, ts, state, direction, speed, predicted_geom,"
        "  next_channel_hint, handover_ready, prediction_confidence)"
        " VALUES"
        " ($1, $2, $3, $4, $5, $6,"
        "  $7, $8, $9, $10, $11, $12,"
        "  ST_SetSRID(ST_MakePoint($13, $14), 0), $15,"
        "  $16, $17, $18,"
        "  CASE WHEN $19::double precision IS NULL OR $20::double precision IS NULL"
        "       THEN NULL"
        "       ELSE ST_SetSRID(ST_MakePoint($19, $20), 0)"
        "  END,"
        "  $21, $22, $23)",
        pqxx::params{
            cfg.camera_id,
            view_channel,
            raw_channel,
            get_value_or_default<int>(track, "object_id", 0),
            display_id,
            global_id,
            get_value_or_default<int>(track, "category", DEFAULT_PERSON_CATEGORY),
            get_optional_value<double>(track, "likelihood"),
            get_optional_value<int>(track, "rect_sx"),
            get_optional_value<int>(track, "rect_sy"),
            get_optional_value<int>(track, "rect_ex"),
            get_optional_value<int>(track, "rect_ey"),
            get_value_or_default<double>(track, "x", 0.0),
            get_value_or_default<double>(track, "y", 0.0),
            timestamp,
            get_value_or_default<std::string>(track, "state", "active"),
            get_value_or_default<std::string>(track, "direction", "UNKNOWN"),
            get_value_or_default<double>(track, "speed", 0.0),
            get_optional_value<double>(track, "predicted_x"),
            get_optional_value<double>(track, "predicted_y"),
            get_value_or_default<int>(track, "next_channel_hint", -1),
            get_value_or_default<bool>(track, "handover_ready", false),
            get_value_or_default<double>(track, "prediction_confidence", 0.0)
        });
}

std::optional<int> lookup_zone_id_in_transaction(pqxx::work& tx,
                                                 int camera_id,
                                                 int raw_channel)
{
    const pqxx::result result = tx.exec(
        "SELECT zone_id"
        " FROM zones"
        " WHERE camera_id = $1 AND channel = $2"
        " LIMIT 1",
        pqxx::params{
            camera_id,
            raw_channel
        });

    if (result.empty()) {
        return std::nullopt;
    }

    return result[0][0].as<int>();
}

/**
 * @brief 발밑점 = 박스 아랫변 중앙. 바닥평면 호모그래피의 입력은 이것이다.
 *
 * 카메라가 주는 `x,y` 는 **박스 무게중심**이다 — CAMERA_API_v15 예시로 확인:
 * rect 1830~2050 × 700~1300 일 때 x,y = 1940,1000 으로 정확히 중앙이다.
 * 무게중심은 가슴 높이라, 바닥평면 H 에 넣으면 그 점이 바닥에 있다고 가정되어
 * **사람이 카메라에서 멀어지는 쪽으로 계통적으로 밀린다.**
 * (`docs/FLOOR_CALIBRATION_RESEARCH.md` §2.4 가 규칙을 못 박아 뒀다)
 *
 * ⚠ **`track_path` 에는 `rect_*` 컬럼이 없다**(`schema.sql:325`). `detections`
 * 는 rect 원본을 같이 저장해 읽는 쪽에서 언제든 다시 계산할 수 있지만, 여기는
 * `geom` 뿐이라 **한 번 잘못 넣으면 소급 복구가 안 된다.** 그래서 detections
 * 와 달리 이쪽은 쓰는 시점에 발밑으로 굳혀 넣는다.
 *
 * rect 가 없으면 x,y 로 폴백한다 — 틀린 값이지만 행을 잃는 것보다 낫고,
 * 예전에 쌓인 데이터와 의미가 같아 최소한 일관된다.
 */
std::pair<double, double> foot_point(const json& track)
{
    const std::optional<int> sx = get_optional_value<int>(track, "rect_sx");
    const std::optional<int> ex = get_optional_value<int>(track, "rect_ex");
    const std::optional<int> ey = get_optional_value<int>(track, "rect_ey");

    if (sx && ex && ey) {
        return { (static_cast<double>(*sx) + static_cast<double>(*ex)) / 2.0,
                 static_cast<double>(*ey) };
    }

    return { get_value_or_default<double>(track, "x", 0.0),
             get_value_or_default<double>(track, "y", 0.0) };
}

void insert_track_path(pqxx::work& tx,
                       const Config& cfg,
                       const json& track,
                       long long track_id,
                       const std::string& timestamp)
{
    const int raw_channel = get_raw_channel(track);
    const int view_channel = get_view_channel(track);
    const std::optional<int> zone_id =
    lookup_zone_id_in_transaction(tx, cfg.camera_id, raw_channel);

    // geom 은 무게중심이 아니라 발밑이다 — 근거는 foot_point 주석
    const auto [foot_x, foot_y] = foot_point(track);

    tx.exec(
        "INSERT INTO track_path"
        " (track_id, display_id, camera_id, channel, ts, geom, zone_id,"
        "  state, direction, speed, predicted_geom)"
        " VALUES"
        " ($1, $2, $3, $4, $5, ST_SetSRID(ST_MakePoint($6, $7), 0), $8,"
        "  $9, $10, $11,"
        "  CASE WHEN $12::double precision IS NULL OR $13::double precision IS NULL"
        "       THEN NULL"
        "       ELSE ST_SetSRID(ST_MakePoint($12, $13), 0)"
        "  END)"
        " ON CONFLICT (track_id, ts) DO NOTHING",
        pqxx::params{
            track_id,
            resolve_display_id(track),
            cfg.camera_id,
            view_channel,
            timestamp,
            foot_x,
            foot_y,
            zone_id,
            get_value_or_default<std::string>(track, "state", "active"),
            get_value_or_default<std::string>(track, "direction", "UNKNOWN"),
            get_value_or_default<double>(track, "speed", 0.0),
            get_optional_value<double>(track, "predicted_x"),
            get_optional_value<double>(track, "predicted_y")
        });
}

}  // namespace

void pollTracks(const Config& cfg, pqxx::connection& db)
{
    const HttpResp response = httpGet(cfg, cfg.tracks());

    if (!response.ok()) {
        std::cerr << "[tracks] http " << response.code << "\n";
        return;
    }

    json body;
    try {
        body = json::parse(response.body);
    } catch (const std::exception& e) {
        std::cerr << "[tracks] json parse fail: " << e.what() << "\n";
        return;
    }

    if (!body.contains("tracks") || !body["tracks"].is_array()) {
        std::cerr << "[tracks] invalid response: tracks array not found\n";
        return;
    }

    const std::string served_utc = body.value("served_utc", "");
    int row_count = 0;
    int skipped = 0;

    try {
        pqxx::work tx(db);

        for (const auto& track : body["tracks"]) {
            // 신원은 **global_id** 기준이다. 예전엔 object_id > 0 도 요구했는데,
            // WiseAI 가 object_id 를 안 실어 보내던 동안 **모든 행이 조용히
            // 버려졌다** — 폴러는 정상으로 보이고 DB만 비어 있었다.
            // object_id 는 이제 참고값으로만 저장한다.
            const long long global_id = resolve_global_id(track);
            const long long display_id = resolve_display_id(track);
            const std::string timestamp = get_value_or_default<std::string>(track, "ts", served_utc);

            if (global_id <= 0 || display_id <= 0 || timestamp.empty()) {
                ++skipped;
                continue;
            }

            const long long track_id = upsert_track(tx, cfg, track, timestamp);

            insert_detection(tx, cfg, track, timestamp);
            insert_track_path(tx, cfg, track, track_id, timestamp);

            ++row_count;
        }

        tx.commit();
    } catch (const std::exception& e) {
        std::cerr << "[tracks] db error: " << e.what() << "\n";
        return;
    }

    // 버린 개수를 반드시 남긴다 — 조용히 0행이던 시절을 두 번 겪지 않게.
    std::cout << "[tracks] +" << row_count << " rows";
    if (skipped > 0) {
        std::cout << " (skipped " << skipped << " — global_id/ts 없음)";
    }
    std::cout << "\n";
}