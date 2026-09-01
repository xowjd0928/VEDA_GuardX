#pragma once

#include <QString>

/**
 * @brief The broadcast microphone pipeline, in one place
 *
 * BroadcastRtpSender owns the pipeline's lifetime; this header owns its
 * *shape*. They are split so the benchmark harness
 * (benchmark/vmsmic/bench_vmsmic.cpp) can measure the pipeline the VMS
 * actually broadcasts through instead of a lookalike written from the
 * comments - a benchmark of a different pipeline is worse than no
 * benchmark, because the numbers still look plausible.
 *
 * Everything here is a pure string builder plus a plugin probe. No element
 * is created, so it is safe to call before a pipeline exists (after
 * gst_init).
 */
namespace guardx {
namespace broadcast {

/** @brief Is this GStreamer element installed? plugins-bad varies by host. */
bool has_element(const char *factory);

/// Broadcast output level when nothing has been configured yet.
constexpr int kVolumeDefaultPercent = 70;

/**
 * @brief Build the mic -> Opus/RTP/UDP description.
 *
 * @param volume_percent broadcast output level, 0..100. Applied inside this
 *        pipeline, which is what keeps it clear of the siren: the siren is
 *        a local file played by RPi C and never passes through here, so
 *        turning the broadcast down cannot quieten an alarm.
 * @param sink_override when non-empty, replaces the trailing udpsink. The
 *        benchmark passes "fakesink" to time encoding without putting real
 *        packets on the network; production leaves it empty.
 * @param backend_out   receives the noise-suppression stage actually
 *        chosen ("webrtcdsp", "highpass+gate", "none"), which depends on
 *        what is installed and therefore belongs in any measurement.
 */
QString build_pipeline(const QString &host, int port, int bitrate,
                       bool denoise, bool agc,
                       int volume_percent = kVolumeDefaultPercent,
                       const QString &sink_override = QString(),
                       QString *backend_out = nullptr);

/// Expander ratio of the fallback noise gate. 1.0 = pass-through (denoise off).
constexpr double kGateRatioOn = 0.25;
constexpr double kGateRatioOff = 1.0;

/**
 * @brief Percent -> linear gain for the `volume` element.
 *
 * Linear rather than cubic: the slider defaults to 70, and a cubic curve
 * would put that at about -9 dB, which reads as "the broadcast is broken"
 * on first launch. Linear keeps 100 at unity - the stage never boosts, so
 * it cannot push the encoder into clipping either.
 */
inline double volume_gain(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    return double(percent) / 100.0;
}

}  // namespace broadcast
}  // namespace guardx
