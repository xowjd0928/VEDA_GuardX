#pragma once
// track_plan.hpp - camera pixels -> LED floor-plan coordinates, and the
// frame the STM32 receives. Pure functions: no DB, no MQTT, no clock.
//
// This logic used to live in the anonymous namespace of
// src/MqttDb/task_track_display.cpp, where nothing could reach it. It is
// the one place where a mistake is invisible until someone stands in front
// of the LED wall - a point in the wrong quadrant, or a frame the STM32
// rejects because a coordinate rounded past 1000 - so it is worth being
// able to check it without hardware (test/track_plan_test.cpp).
//
// Extracting it changed no behaviour: the constants, the rounding and the
// A/B rules below are the ones that were already running.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace guardx {
namespace track_plan {

// Camera source resolution. detections.geom / predicted_geom live in this
// coordinate space (the /tracks bbox centre in pixels). Must match
// FRAME_W/FRAME_H in vms/track_history.h.
inline constexpr double FRAME_W = 2592.0;
inline constexpr double FRAME_H = 1520.0;

// LED floor plan: STM32 screen 2 is one room split 2x2, the whole thing
// spanning 0~1000. One channel is one cell, laid out like the VMS FLOOR MAP
// (tracking_panel.cpp cell_of): row = ch/2, col = ch%2.
//
// Points are kept inside the middle 10~90% of a cell, also like FLOOR MAP -
// a 3x3 pixel dot flush against a wall eats the wall line and you can no
// longer tell which cell it is in.
inline constexpr int CELL = 500;        // cell edge (1000 split in two)
inline constexpr int MARGIN = 50;       // inset inside a cell (10%)
inline constexpr int SPAN = 400;        // width a dot actually moves in (80%)
inline constexpr int COORD_MAX = 1000;

struct Quadrant {
    int col;
    int row;
};

// !!! needs to be confirmed on site !!!
// Which floor-plan cell the camera on CH1 is looking at depends on where
// the cameras hang; code cannot know it. The repo already holds two
// conflicting records (vms/fire_zone_map.h says zone1 -> ch0, the comment
// in vms/zone_config.h says the measured value is zone1 -> ch1). Screen
// channel order is kept as-is: if the dot lands in the wrong cell, this
// table is the only thing to fix.
inline constexpr Quadrant QUADRANT[4] = {
    {0, 0},   // ch0 -> top-left
    {1, 0},   // ch1 -> top-right
    {0, 1},   // ch2 -> bottom-left
    {1, 1},   // ch3 -> bottom-right
};
inline constexpr int CHANNEL_COUNT = 4;

// Below this distance the predicted point is not drawn (floor-plan units).
// A dot is 3x3 pixels, so anything closer reads as one smeared dot rather
// than as a direction.
inline constexpr int MIN_AB_GAP = 25;

// Same values as INTRUDER_*_VALID in the STM32 modbus_slave.h.
inline constexpr int STATUS_A = 1 << 0;
inline constexpr int STATUS_B = 1 << 1;

/** @brief Camera pixels -> floor plan 0~1000. False if the channel is out of range. */
inline bool toPlan(int channel, double px, double py, int& out_x, int& out_y)
{
    if (channel < 0 || channel >= CHANNEL_COUNT) return false;

    const double fx = std::clamp(px / FRAME_W, 0.0, 1.0);
    const double fy = std::clamp(py / FRAME_H, 0.0, 1.0);
    const Quadrant& q = QUADRANT[channel];

    out_x = q.col * CELL + MARGIN + (int)std::lround(SPAN * fx);
    out_y = q.row * CELL + MARGIN + (int)std::lround(SPAN * fy);

    // Squeeze once more so rounding cannot push past the edge. The STM32
    // rejects anything over 1000 as an illegal-value exception (0x03), and
    // that throws away the whole frame, not just the bad coordinate.
    out_x = std::clamp(out_x, 0, COORD_MAX);
    out_y = std::clamp(out_y, 0, COORD_MAX);
    return true;
}

/** @brief What one publish carries: status bits plus the two points. */
struct Frame {
    int status = 0;
    int ax = 0;
    int ay = 0;
    int bx = 0;
    int by = 0;
};

/** @brief The frame that clears both dots. */
inline Frame clearFrame() { return Frame{}; }

/**
 * @brief Build the frame for one detection row.
 *
 * @param channel     raw_channel of the row, not the channel the VMS
 *                    pointed at - a target found by global_id may have
 *                    walked into another camera since.
 * @param has_pred    predicted_geom was non-NULL
 * @param direction   detections.direction ("UNKNOWN" when NULL)
 * @return false if the current point cannot be mapped; nothing is sent then.
 */
inline bool buildFrame(int channel, double cx, double cy, bool has_pred,
                       double pred_x, double pred_y,
                       const std::string& direction, Frame& out)
{
    out = Frame{};
    if (!toPlan(channel, cx, cy, out.ax, out.ay)) return false;
    out.status = STATUS_A;

    if (!has_pred || direction == "STOP") return true;

    int px = 0, py = 0;
    if (!toPlan(channel, pred_x, pred_y, px, py)) return true;

    // The camera fills predicted with the current position when it has no
    // prediction (track_manager.cc), so a NULL check is not enough - the
    // points have to actually be apart.
    const int dx = px - out.ax;
    const int dy = py - out.ay;
    if (dx * dx + dy * dy < MIN_AB_GAP * MIN_AB_GAP) return true;

    out.bx = px;
    out.by = py;
    out.status |= STATUS_B;
    return true;
}

/** @brief The exact JSON RPi C parses (matrix_link.h, payload 1). */
inline std::string buildPayload(long long timestamp_ms, long seq,
                                const Frame& f)
{
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "{\"node_id\":\"rpib\",\"timestamp\":%lld,\"seq\":%ld,"
                  "\"status\":%d,\"ax\":%d,\"ay\":%d,\"bx\":%d,\"by\":%d}",
                  timestamp_ms, seq, f.status, f.ax, f.ay, f.bx, f.by);
    return buf;
}

}  // namespace track_plan
}  // namespace guardx
