// bench_vmsmic.cpp - VMS broadcast microphone path benchmark.
//
// Measures the pipeline the VMS actually broadcasts through: the
// description comes from vms/broadcast_pipeline.h, not from a copy. What is
// measured needs no labelled audio, so it can run today:
//
//   startup      set_state(PLAYING) -> first encoded packet leaves
//   encode       last needed input sample arrives -> Opus packet emitted
//   cadence      interval between outgoing packets, and its jitter
//   throughput   packets/s and the bitrate actually produced
//   latency      what the pipeline reports for GST_QUERY_LATENCY
//   cost         CPU seconds and peak RSS of this process
//
// Accuracy (does it detect the right thing) is NOT measured here - that
// needs labelled clips and belongs to the TOIMIC harness. See
// benchmark/README.md for where a dataset plugs in.
//
// Build: it is a target of the VMS CMake project (bench_vmsmic), created
// only when GStreamer is found. Run:
//   ./bench_vmsmic --seconds 20
//   ./bench_vmsmic --describe          # print the pipeline and exit
//   ./bench_vmsmic --udp --host 172.20.33.114 --port 5004
//
// Without a microphone the run fails at PLAYING and says so - that is the
// "tool is wired up, hardware is missing" outcome, not a silent zero.

#include "broadcast_pipeline.h"
#include "bench_sysstat.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTextStream>

#include <gst/gst.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <vector>

namespace {

QTextStream &out()
{
    static QTextStream s(stdout);
    return s;
}

/** @brief Wall clock in nanoseconds - one base for every probe. */
qint64 now_ns()
{
    return qint64(g_get_monotonic_time()) * 1000;
}

struct Stats {
    double min = 0, max = 0, mean = 0, p50 = 0, p95 = 0, p99 = 0;
    int n = 0;
};

Stats summarise(std::vector<double> v)
{
    Stats s;
    s.n = int(v.size());
    if (v.empty())
        return s;
    std::sort(v.begin(), v.end());
    s.min = v.front();
    s.max = v.back();
    double sum = 0;
    for (double x : v) sum += x;
    s.mean = sum / double(v.size());
    auto at = [&v](double q) {
        const size_t i = size_t(q * double(v.size() - 1) + 0.5);
        return v[std::min(i, v.size() - 1)];
    };
    s.p50 = at(0.50);
    s.p95 = at(0.95);
    s.p99 = at(0.99);
    return s;
}

void print_stats(const char *label, const char *unit, const Stats &s)
{
    if (s.n == 0) {
        out() << QString("  %1  (no samples)\n").arg(QString(label), -22);
        return;
    }
    out() << QString("  %1  n=%2  min %3  p50 %4  p95 %5  p99 %6  max %7  %8\n")
                 .arg(QString(label), -22)
                 .arg(s.n, 5)
                 .arg(s.min, 8, 'f', 2)
                 .arg(s.p50, 8, 'f', 2)
                 .arg(s.p95, 8, 'f', 2)
                 .arg(s.p99, 8, 'f', 2)
                 .arg(s.max, 8, 'f', 2)
                 .arg(QString(unit));
}

// ---------------------------------------------------------------- collector

struct Collector {
    std::mutex mtx;

    // Arrival wall clock of raw audio, keyed by its presentation time. The
    // encoder emits one packet per 20 ms of input, so an output packet is
    // matched to the newest input it must have consumed. That is an
    // approximation - input buffers are not 20 ms aligned - but it bounds
    // the encode stage from above, which is the number that matters.
    std::map<GstClockTime, qint64> in_arrival;

    std::vector<double> encode_ms;
    std::vector<double> gap_ms;
    qint64 first_out_ns = 0;
    qint64 last_out_ns = 0;
    qint64 total_bytes = 0;
    int packets = 0;
};

Collector g_col;
qint64 g_playing_ns = 0;

GstPadProbeReturn on_enc_in(GstPad *, GstPadProbeInfo *info, gpointer)
{
    GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buf)
        return GST_PAD_PROBE_OK;
    const GstClockTime pts = GST_BUFFER_PTS(buf);
    if (!GST_CLOCK_TIME_IS_VALID(pts))
        return GST_PAD_PROBE_OK;

    std::lock_guard<std::mutex> lk(g_col.mtx);
    g_col.in_arrival[pts] = now_ns();
    return GST_PAD_PROBE_OK;
}

GstPadProbeReturn on_out(GstPad *, GstPadProbeInfo *info, gpointer)
{
    GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buf)
        return GST_PAD_PROBE_OK;

    const qint64 t = now_ns();
    const GstClockTime pts = GST_BUFFER_PTS(buf);
    const GstClockTime dur = GST_BUFFER_DURATION(buf);
    const gsize size = gst_buffer_get_size(buf);

    std::lock_guard<std::mutex> lk(g_col.mtx);

    if (g_col.first_out_ns == 0)
        g_col.first_out_ns = t;
    else
        g_col.gap_ms.push_back(double(t - g_col.last_out_ns) / 1e6);
    g_col.last_out_ns = t;
    g_col.total_bytes += qint64(size);
    ++g_col.packets;

    if (GST_CLOCK_TIME_IS_VALID(pts)) {
        // Everything older than PTS + duration has been consumed, so it is
        // dropped here too - otherwise the map grows for the whole run.
        const GstClockTime edge = pts + (GST_CLOCK_TIME_IS_VALID(dur) ? dur : 0);
        auto it = g_col.in_arrival.upper_bound(edge);
        if (it != g_col.in_arrival.begin()) {
            --it;
            g_col.encode_ms.push_back(double(t - it->second) / 1e6);
            g_col.in_arrival.erase(g_col.in_arrival.begin(), it);
        }
    }
    return GST_PAD_PROBE_OK;
}

bool attach_probe(GstElement *pipeline, const char *element,
                  const char *pad_name, GstPadProbeCallback cb)
{
    GstElement *e = gst_bin_get_by_name(GST_BIN(pipeline), element);
    if (!e) {
        out() << QString("  ! element \"%1\" is not in the pipeline - "
                         "that measurement is skipped\n").arg(QString(element));
        return false;
    }
    GstPad *pad = gst_element_get_static_pad(e, pad_name);
    if (pad) {
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, cb, nullptr, nullptr);
        gst_object_unref(pad);
    }
    gst_object_unref(e);
    return pad != nullptr;
}

QString arg_value(const QStringList &args, const QString &key,
                  const QString &def)
{
    const int i = args.indexOf(key);
    if (i >= 0 && i + 1 < args.size())
        return args.at(i + 1);
    return def;
}

}  // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList args = QCoreApplication::arguments();

    const int seconds = arg_value(args, "--seconds", "20").toInt();
    const int bitrate = arg_value(args, "--bitrate", "64000").toInt();
    const QString host = arg_value(args, "--host", "127.0.0.1");
    const int port = arg_value(args, "--port", "5004").toInt();
    const bool denoise = !args.contains("--no-denoise");
    // 음량은 인코더 앞단의 곱셈이라 지연에는 거의 영향이 없지만, 낮은
    // 음량은 VBR 비트레이트를 눈에 띄게 낮춘다 — 비교할 때 같은 값으로.
    const int volume = arg_value(args, "--volume", "70").toInt();
    const bool agc = args.contains("--agc");
    const bool describe = args.contains("--describe");
    // Default sink is fakesink. The point is to measure capture and
    // encoding, and putting real RTP on the wire during a benchmark can
    // reach a live speaker in the field.
    const bool udp = args.contains("--udp");
    const QString json_path = arg_value(args, "--json", QString());

    gst_init(nullptr, nullptr);

    QString backend;
    const QString desc = guardx::broadcast::build_pipeline(
        host, port, bitrate, denoise, agc, volume,
        udp ? QString()
            : QStringLiteral("fakesink name=sink sync=false async=false"),
        &backend);

    out() << "GuardX VMSMIC benchmark\n";
    out() << "  denoise backend : " << backend << "\n";
    out() << "  volume          : " << volume << " %\n";
    out() << "  sink            : "
          << (udp ? QString("udp %1:%2").arg(host).arg(port)
                  : QString("fakesink")) << "\n";
    out() << "  pipeline        : " << desc << "\n";
    out().flush();

    if (describe)
        return 0;

    GError *err = nullptr;
    GstElement *pipeline = gst_parse_launch(desc.toUtf8().constData(), &err);
    if (!pipeline || err) {
        out() << "  ! pipeline build failed: "
              << QString(err ? err->message : "unknown") << "\n";
        out().flush();
        if (err)
            g_error_free(err);
        if (pipeline)
            gst_object_unref(pipeline);
        return 1;
    }

    attach_probe(pipeline, "enc", "sink", on_enc_in);
    attach_probe(pipeline, "sink", "sink", on_out);

    g_playing_ns = now_ns();
    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE) {
        out() << "  ! could not start the pipeline (no microphone?)\n";
        out().flush();
        gst_object_unref(pipeline);
        return 1;
    }

    // Wait for the state change so a missing capture device is reported
    // here rather than as an empty result at the end.
    GstState state = GST_STATE_NULL;
    if (gst_element_get_state(pipeline, &state, nullptr, 5 * GST_SECOND) ==
            GST_STATE_CHANGE_FAILURE ||
        state != GST_STATE_PLAYING) {
        out() << "  ! pipeline did not reach PLAYING (no microphone?)\n";
        out().flush();
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return 1;
    }

    out() << QString("  running %1 s ...\n").arg(seconds);
    out().flush();

    GstBus *bus = gst_element_get_bus(pipeline);
    const qint64 deadline = now_ns() + qint64(seconds) * 1000000000LL;
    bool fatal = false;
    while (now_ns() < deadline && !fatal) {
        GstMessage *msg = gst_bus_timed_pop_filtered(
            bus, 100 * GST_MSECOND,
            GstMessageType(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
        if (!msg)
            continue;
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            GError *e = nullptr;
            gchar *dbg = nullptr;
            gst_message_parse_error(msg, &e, &dbg);
            out() << "  ! " << QString(e ? e->message : "error") << "\n";
            if (e)
                g_error_free(e);
            g_free(dbg);
            fatal = true;
        } else {
            fatal = true;   // EOS - the source went away
        }
        gst_message_unref(msg);
    }

    // Pipeline latency is a property of the running graph, so ask before
    // tearing it down.
    double latency_min_ms = -1, latency_max_ms = -1;
    {
        GstQuery *q = gst_query_new_latency();
        if (gst_element_query(pipeline, q)) {
            gboolean live = FALSE;
            GstClockTime lo = 0, hi = 0;
            gst_query_parse_latency(q, &live, &lo, &hi);
            if (GST_CLOCK_TIME_IS_VALID(lo))
                latency_min_ms = double(lo) / 1e6;
            if (GST_CLOCK_TIME_IS_VALID(hi))
                latency_max_ms = double(hi) / 1e6;
        }
        gst_query_unref(q);
    }

    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    // ------------------------------------------------------------- report
    Stats enc_s, gap_s;
    qint64 packets = 0, bytes = 0, startup_ns = 0, span_ns = 0;
    {
        std::lock_guard<std::mutex> lk(g_col.mtx);
        enc_s = summarise(g_col.encode_ms);
        gap_s = summarise(g_col.gap_ms);
        packets = g_col.packets;
        bytes = g_col.total_bytes;
        startup_ns = g_col.first_out_ns ? g_col.first_out_ns - g_playing_ns : 0;
        span_ns = g_col.first_out_ns ? g_col.last_out_ns - g_col.first_out_ns : 0;
    }

    const bench::SysStat sys = bench::sample();
    const double span_s = double(span_ns) / 1e9;
    const double kbps = span_s > 0 ? double(bytes) * 8.0 / span_s / 1000.0 : 0;
    const double pps = span_s > 0 ? double(packets - 1) / span_s : 0;

    out() << "\nresults\n";
    out() << QString("  startup to first packet   %1 ms\n")
                 .arg(double(startup_ns) / 1e6, 0, 'f', 1);
    print_stats("encode latency", "ms", enc_s);
    print_stats("packet interval", "ms", gap_s);
    out() << QString("  packets                   %1  (%2 /s)\n")
                 .arg(packets)
                 .arg(pps, 0, 'f', 1);
    out() << QString("  payload bitrate           %1 kbps (target %2)\n")
                 .arg(kbps, 0, 'f', 1)
                 .arg(bitrate / 1000);
    // A live source usually reports no upper bound, which arrives as
    // CLOCK_TIME_NONE. Printing it as -1 ms reads like a measurement.
    out() << QString("  reported latency          %1 .. %2\n")
                 .arg(latency_min_ms < 0 ? QString("n/a")
                                         : QString::number(latency_min_ms, 'f', 1))
                 .arg(latency_max_ms < 0
                          ? QString("unbounded")
                          : QString::number(latency_max_ms, 'f', 1) + " ms");
    out() << QString("  cpu                       %1 s user + %2 s sys "
                     "(%3 % of one core)\n")
                 .arg(sys.cpu_user_s, 0, 'f', 2)
                 .arg(sys.cpu_sys_s, 0, 'f', 2)
                 .arg(span_s > 0
                          ? (sys.cpu_user_s + sys.cpu_sys_s) / span_s * 100.0
                          : 0.0,
                      0, 'f', 1);
    out() << QString("  rss                       %1 MB now, %2 MB peak\n")
                 .arg(sys.rss_now_mb, 0, 'f', 1)
                 .arg(sys.rss_peak_mb, 0, 'f', 1);
    out().flush();

    if (!json_path.isEmpty()) {
        QJsonObject o;
        o["target"] = "vmsmic";
        o["taken_at"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        o["denoise_backend"] = backend;
        o["bitrate_target"] = bitrate;
        o["volume_percent"] = volume;
        o["seconds"] = seconds;
        o["startup_ms"] = double(startup_ns) / 1e6;
        o["encode_ms_p50"] = enc_s.p50;
        o["encode_ms_p95"] = enc_s.p95;
        o["encode_ms_max"] = enc_s.max;
        o["packet_interval_ms_p50"] = gap_s.p50;
        o["packet_interval_ms_p95"] = gap_s.p95;
        o["packets"] = double(packets);
        o["kbps"] = kbps;
        o["latency_min_ms"] = latency_min_ms;
        o["latency_max_ms"] = latency_max_ms;
        o["cpu_user_s"] = sys.cpu_user_s;
        o["cpu_sys_s"] = sys.cpu_sys_s;
        o["rss_peak_mb"] = sys.rss_peak_mb;
        QFile f(json_path);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
            out() << "  wrote " << json_path << "\n";
            out().flush();
        }
    }

    return fatal ? 1 : 0;
}
