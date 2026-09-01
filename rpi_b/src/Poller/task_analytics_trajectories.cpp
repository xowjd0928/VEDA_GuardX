#include "Poller/task_analytics_trajectories.hpp"

#include "Poller/http_client.hpp"

#include <nlohmann/json.hpp>

#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

using json = nlohmann::json;

namespace {

template <typename T>
T get_value_or_default(const json& object, const char* key, T default_value)
{
    const auto iter = object.find(key);
    if (iter == object.end() || iter->is_null()) {
        return default_value;
    }

    return iter->get<T>();
}

int get_raw_channel(const json& trajectory)
{
    if (trajectory.contains("raw_channel") && !trajectory["raw_channel"].is_null()) {
        return trajectory["raw_channel"].get<int>();
    }

    if (trajectory.contains("channel") && !trajectory["channel"].is_null()) {
        return trajectory["channel"].get<int>() - 1;
    }

    return 0;
}

int get_view_channel(const json& trajectory)
{
    if (trajectory.contains("channel") && !trajectory["channel"].is_null()) {
        return trajectory["channel"].get<int>();
    }

    return get_raw_channel(trajectory) + 1;
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

std::optional<int> resolve_zone_id(pqxx::work& tx,
                                   const Config& cfg,
                                   const json& trajectory)
{
    const int raw_channel = get_raw_channel(trajectory);
    const std::optional<int> zone_id =
        lookup_zone_id_in_transaction(tx, cfg.camera_id, raw_channel);

    if (zone_id.has_value()) {
        return zone_id;
    }

    if (trajectory.contains("zone_id") && !trajectory["zone_id"].is_null()) {
        return trajectory["zone_id"].get<int>();
    }

    return std::nullopt;
}

void upsert_trajectory_segment(pqxx::work& tx,
                               const Config& cfg,
                               const json& trajectory,
                               const std::string& served_at)
{
    const std::string segment_ts =
        get_value_or_default<std::string>(trajectory, "ts", served_at);

    if (segment_ts.empty()) {
        throw std::runtime_error("trajectory timestamp is empty");
    }

    const int camera_id = cfg.camera_id;
    const int raw_channel = get_raw_channel(trajectory);
    const int view_channel = get_view_channel(trajectory);
    const std::optional<int> zone_id = resolve_zone_id(tx, cfg, trajectory);

    tx.exec(
        "INSERT INTO trajectory_segments"
        " (camera_id, segment_id, global_id, display_id, object_id,"
        "  channel, raw_channel, zone_id,"
        "  start_ms, end_ms, dwell_ms,"
        "  confidence, is_reliable, state, segment_ts, served_at)"
        " VALUES"
        " ($1, $2, $3, $4, $5,"
        "  $6, $7, $8,"
        "  $9, $10, $11,"
        "  $12, $13, $14, $15, $16)"
        " ON CONFLICT (camera_id, segment_id, start_ms)"
        " DO UPDATE SET"
        "   global_id = EXCLUDED.global_id,"
        "   display_id = EXCLUDED.display_id,"
        "   object_id = EXCLUDED.object_id,"
        "   channel = EXCLUDED.channel,"
        "   raw_channel = EXCLUDED.raw_channel,"
        "   zone_id = EXCLUDED.zone_id,"
        "   start_ms = EXCLUDED.start_ms,"
        "   end_ms = EXCLUDED.end_ms,"
        "   dwell_ms = EXCLUDED.dwell_ms,"
        "   confidence = EXCLUDED.confidence,"
        "   is_reliable = EXCLUDED.is_reliable,"
        "   state = EXCLUDED.state,"
        "   segment_ts = EXCLUDED.segment_ts,"
        "   served_at = EXCLUDED.served_at",
        pqxx::params{
            camera_id,
            get_value_or_default<long long>(trajectory, "segment_id", 0),
            get_value_or_default<long long>(trajectory, "global_id", 0),
            get_value_or_default<long long>(trajectory, "display_id", 0),
            get_value_or_default<int>(trajectory, "object_id", 0),
            view_channel,
            raw_channel,
            zone_id,
            get_value_or_default<long long>(trajectory, "start_ms", 0),
            get_value_or_default<long long>(trajectory, "end_ms", 0),
            get_value_or_default<long long>(trajectory, "dwell_ms", 0),
            get_value_or_default<double>(trajectory, "confidence", 0.0),
            get_value_or_default<bool>(trajectory, "is_reliable", false),
            get_value_or_default<std::string>(trajectory, "state", "completed"),
            segment_ts,
            served_at.empty() ? segment_ts : served_at
        });
}

const json* find_trajectory_array(const json& body)
{
    if (body.is_array()) {
        return &body;
    }

    const auto iter = body.find("trajectories");
    if (iter != body.end() && iter->is_array()) {
        return &(*iter);
    }

    return nullptr;
}

}  // namespace

void pollAnalyticsTrajectories(const Config& cfg, pqxx::connection& db)
{
    const HttpResp response = httpGet(cfg, cfg.trajectories());

    if (!response.ok()) {
        std::cerr << "[trajectory] http " << response.code << "\n";
        return;
    }

    json body;
    try {
        body = json::parse(response.body);
    } catch (const std::exception& e) {
        std::cerr << "[trajectory] json parse fail: " << e.what() << "\n";
        return;
    }

    const json* trajectories = find_trajectory_array(body);
    if (trajectories == nullptr) {
        std::cerr << "[trajectory] invalid response: trajectories array not found\n";
        return;
    }

    const std::string served_at = body.is_object()
        ? body.value("served_utc", "")
        : std::string();

    int row_count = 0;
    int skipped = 0;

    try {
        pqxx::work tx(db);

        for (const auto& trajectory : *trajectories) {
            const long long segment_id =
                get_value_or_default<long long>(trajectory, "segment_id", 0);
            const long long global_id =
                get_value_or_default<long long>(trajectory, "global_id", 0);

            if (segment_id <= 0 || global_id <= 0) {
                ++skipped;
                continue;
            }

            upsert_trajectory_segment(tx, cfg, trajectory, served_at);
            ++row_count;
        }

        tx.commit();
    } catch (const std::exception& e) {
        std::cerr << "[trajectory] db error: " << e.what() << "\n";
        return;
    }

    std::cout << "[trajectory] +" << row_count << " rows";
    if (skipped > 0) {
        std::cout << " (skipped " << skipped << " invalid rows)";
    }
    std::cout << "\n";
}
