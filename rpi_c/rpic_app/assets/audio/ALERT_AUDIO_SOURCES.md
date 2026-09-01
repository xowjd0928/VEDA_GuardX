# Alert audio sources

The three files were downloaded from Pixabay and converted to 48 kHz, 16-bit
stereo PCM WAV for Raspberry Pi playback. Pixabay marks these sources as free
for use under the Pixabay Content License.

License: https://pixabay.com/service/license-summary/

| Local file | Purpose | Source | Creator |
| --- | --- | --- | --- |
| `fire_alert.wav` | Fire-truck siren | [A loud, wailing emergency siren from a fire truck](https://pixabay.com/sound-effects/film-special-effects-a-loud-wailing-emergency-siren-from-a-fire-truck-535504/) | haruudu |
| `intruder_alert.wav` | Intrusion/security alert | [Security Alarm](https://pixabay.com/sound-effects/film-special-effects-security-alarm-80493/) | JSilverSound (Freesound) |
| `crowd_alert.wav` | Congestion/crowd warning | [Warning Alarm Loop #2](https://pixabay.com/sound-effects/film-special-effects-warning-alarm-loop-2-314878/) | Audley_Fergine |

Downloaded and converted on 2026-08-25.

`crowd_alert.wav` repeats the licensed source loop twice and boosts the
420 Hz–4.5 kHz presence band so it remains audible on the small RPi speaker.
