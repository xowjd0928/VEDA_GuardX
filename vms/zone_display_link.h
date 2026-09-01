#pragma once

#include <QHash>
#include <QObject>

/**
 * @brief Pushes dummy-zone temperature/humidity to the field LED matrix
 *
 * The LED floor plan gets its zone readings from RPi B
 * (`rpib_decision`, publish_zone_display()). RPi B only knows zones that
 * have real hardware: a dummy zone never produces a sensor message, so it
 * never reaches that function and its LED cells keep the STM32 reset value
 * ("no data") forever.
 *
 * ZoneSensorStore already synthesises plausible readings for those zones so
 * the VMS screens are not blank. This class forwards exactly those values to
 * the same display topic RPi B uses, which lets the
 * VMS -> RPi C -> Modbus -> STM32 path be exercised end to end without
 * waiting for the missing sensors.
 *
 * **Real zones are never published from here.** Zone 1 has hardware and
 * RPi B owns its topic; a second publisher on a retained topic would make
 * the LED flip between the real reading and a synthetic one, and the last
 * writer would win at reconnect. The dummy flag in fire_zone_map.h is the
 * only selector - flip a zone to `dummy = false` there and this class drops
 * it automatically.
 *
 * The payload, the clamping and the "resend even when unchanged" rule are
 * copied from RPi B on purpose. The STM32 registers are unsigned 16-bit and
 * reject out-of-range frames, and a rebooted STM32 falls back to "no data"
 * without telling anyone - so a periodic refresh is what brings the display
 * back, not a value change.
 *
 * Publishing can be turned off with the registry key `display/zone_dummy`
 * (GuardX/VMS) when a site does not want synthetic values on the wall.
 */
class ZoneDisplayLink : public QObject
{
    Q_OBJECT

public:
    static ZoneDisplayLink *instance();

    /**
     * @brief Subscribe to ZoneSensorStore and start forwarding
     *
     * Safe to call more than once; only the first call has an effect.
     * Call it after MqttLink::start() - publishes made while the broker is
     * offline are dropped, not queued, and the periodic refresh covers the
     * gap once the link is back.
     */
    void start();

    bool enabled() const { return m_enabled; }

private:
    explicit ZoneDisplayLink(QObject *parent = nullptr);

    void on_zone_updated(int zone_id);

    /// Same register limits the STM32 exposes (guardx_modbus_regs.h).
    static constexpr int TEMP_X10_MAX = 65534;
    static constexpr int HUMIDITY_MAX = 100;

    /// Resend an unchanged value at least this often, so a rebooted STM32
    /// does not sit at "no data" until the synthetic value happens to move.
    /// Matches DISPLAY_REFRESH_MS in rpib_decision.
    static constexpr qint64 REFRESH_MS = 30000;

    struct Sent {
        int temp_x10 = 0;
        int humidity = 0;
        qint64 last_ms = 0;
        bool valid = false;
    };

    bool m_enabled = true;
    bool m_started = false;
    long m_seq = 0;
    QHash<int, Sent> m_sent;   ///< zone_id -> last accepted publish
};
