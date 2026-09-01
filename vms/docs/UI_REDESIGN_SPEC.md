# GuardX VMS — UI Redesign Spec & Handoff

**Purpose:** enough detail for a fresh session to reskin the Qt VMS to match the provided
control-room design. **Design source:** [`GuardX VMS.dc.html`](GuardX%20VMS.dc.html) in this
folder (a 1920×1080 dark control-room mockup with 6 screens + overlays).

> **2026-08-19 design amendments** (user feedback, commit on `VEDA-199`): the wall is a
> **fixed 2x2 grid** - the FOCUS/fullscreen mode, tile-click zoom, and `fullscreen_profile`
> are deleted; all UI text moved from ALL-CAPS to **Title Case** (acronyms like CH/CPU/GOV
> stay; MQTT/SUNAPI payloads untouched); the nav tabs carry **tooltips**; the **AI Overlay
> toggle is gone** - detection boxes are drawn only for tracked (right-clicked) people; and
> multi-tracking no longer needs Ctrl - every click/right-click **toggles** a target in or
> out of the tracked set. Sections below are amended in place where they were normative.

**Chosen scope (decided 2026-07-23):** **Foundation + Live reskin.**
1. Apply the design system (colors + fonts) globally.
2. Rebuild the app shell — top bar + nav rail (replacing the current `QListWidget` sidebar).
3. Reskin the **Live Monitoring** screen (the only screen with working data) to match.
4. The other 5 screens = navigable **placeholder** pages (real data not wired into the VMS yet).

> Do **not** build the 5 non-Live screens fully — they need backend data (RPi predictions,
> STM32 sensors, tracking) that isn't in the VMS. Stub them behind the nav so navigation works.

---

## 1. Design system (extract these exactly)

### 1.1 Palette

| Semantic | Hex | RGB | Use |
|---|---|---|---|
| `bg0` | `#0A0D12` | 10,13,18 | app background (darkest) |
| `panel` | `#0D1118` | 13,17,24 | panels, top bar, nav rail |
| `elevated` | `#12161F` | 18,22,31 | pills, inputs, progress track, tile hover |
| `elevated2` | `#161C25` | 22,28,37 | active nav bg, row hover |
| `border` | `#1C2330` | 28,35,48 | primary borders |
| `border2` | `#232B38` | 35,43,56 | dividers, control borders |
| `borderDim` | `#161C25` | 22,28,37 | panel-header underline |
| `rowDivider` | `#10141B` | 16,20,27 | list-row separators |
| `textHi` | `#E2E8F2` | 226,232,242 | primary text |
| `textMid` | `#C6CEDB` | 198,206,219 | secondary text |
| `textMuted` | `#8B96A8` | 139,150,168 | labels |
| `textDim` | `#5A6577` | 90,101,119 | captions |
| `textFaint` | `#3A4557` | 58,69,87 | faint meta |
| `accent` | `#4EA1FF` | 78,161,255 | primary accent, links, tracks, boxes |
| `accentHover` | `#7DBBFF` | 125,187,255 | link hover |
| `alarm` | `#FF4A36` | 255,74,54 | fire/alarm accent (configurable) |
| `green` | `#2FD27D` | 47,210,125 | OK / online |
| `amber` | `#FFB224` | 255,178,36 | warning |

**Disaster-mode reds** (for later, screen 6b): panel `#140D0C`, border `#3A1512`,
text `#B08A85`/`#7A5B57`, overlay bg `rgba(8,6,6,.94)`.

**Occupancy → color rule** (used on OCC badges, bars, forecasts):
`load > 0.70 → alarm` · `load > 0.45 → amber` · else `green` (tiles) or `accent` (forecast bars).

### 1.2 Typography

- **UI font:** IBM Plex Sans (weights 400/500/600/700)
- **Data/mono font:** IBM Plex Mono (weights 400/500/600) — used for all numbers, codes,
  timestamps, channel names, endpoints
- **Letter-spacing** is part of the look: headings `.12–.14em`, labels `.06–.10em`

> **Font handling in Qt:** use QSS `font-family: "IBM Plex Sans", "Segoe UI", sans-serif;`
> and `"IBM Plex Mono", "Cascadia Mono", "Consolas", monospace;`. If IBM Plex `.ttf` files
> aren't installed, Qt falls back to Segoe UI / Cascadia — acceptable. To match exactly,
> drop IBM Plex TTFs into a Qt resource and `QFontDatabase::addApplicationFont(...)` at
> startup. IBM Plex is OFL-licensed (free). Keep the family name in one constant so it's
> swappable.

### 1.3 Geometry (from the mockup, 1920×1080 canvas)

| Element | Size |
|---|---|
| Top bar | height **52px** |
| Nav rail | width **74px**; buttons **62×58px** (icon 17px + label 8.5px) |
| Live screen padding | `12px 14px 0` |
| Live header | height **34px** |
| Live grid | 2×2, gap **8px**, tiles `border-radius:2px` |
| Live bottom strip | height **172px** (2 panels, gap 10px) |
| Panel headers | 30–38px, underline `borderDim` |
| Corner radius | **2px** everywhere (sharp, industrial) |

---

## 2. The 5 screens (nav rail order)

| # | id | Nav label / icon | Status for this task |
|---|---|---|---|
| 1 | `live` | LIVE ▦ | **Reskin fully** (working data) |
| 2 | `heat` | CROWD ◉ | Placeholder (needs `/prediction`) |
| 3 | `report` | REPORT ▤ | Placeholder (needs session DB) |
| 4 | `device` | DEVICE ◫ | Placeholder (needs RPi/STM32) |
| 5 | `zone` | ZONES ⚙ | Placeholder (needs zone rules) |

There is no `track` screen. Tracking moved into the LIVE page as the right-hand
`TRACKING · 동선` panel (§3.5) — an operator watching the wall needs the path
*next to* the video, not on another tab. The `⇢ TRACK` nav item is gone.

Overlays (later, not this task): congestion **toast** (top-right, amber) and fire
**disaster overlay** (full-screen red). Design is in the HTML `sc-if disasterOn` / `toastOn`
blocks if needed.

---

## 3. Component specs — what to build

### 3.1 Top bar (`TopBar` : QWidget, 52px)
Left→right: accent `#4EA1FF` 26×26 square with bold "G" → "GUARDX" + muted "VMS 1.0" →
vertical divider → "SITE **TERMINAL WEST · SECTOR B**" → **status pills** (each: 6px colored
dot + mono label; `CAM 4/4` green, `RPI A·B·C` green, `DB Nms` green, `POLL PULL 1.0s` blue) →
stretch → *(FIRE ALARM button — only when alarm active; pulsing red)* → "SIMULATE FIRE EVENT"
outline button → **clock** (KST 15px mono + UTC 10px dim, right-aligned; `QTimer` 1s).

### 3.2 Nav rail (`NavRail` : QWidget, 74px)
5 vertical buttons. **Active:** bg `elevated2`, left-border 2px `accent`, text `textHi`.
**Inactive:** transparent, text `textDim`, hover → `textHi`. Emits `screenSelected(id)`.
Bottom: vertical rotated caption "BIG EYES · GUARDX" in `textFaint`.
Icons can be the unicode glyphs (▦ ◉ ▤ ◫ ⚙) or swapped for real SVG icons later.

### 3.3 Live Monitoring (reskin existing `LiveViewer`)
**Header (34px, amended 08-19):** "Live Monitoring" (700, `.14em`) + stretch +
`Edge Map` toggle button + `Face Blur` toggle switch. The tech caption, "WALL LAYOUT"
segmented buttons, and the "AI OVERLAY" toggle were removed:
- The wall is a **fixed 2x2 grid** - no fullscreen mode, no tile-click zoom, and the
  high-res `fullscreen_profile` is gone with it (grid-4MP violations became impossible).
- Detection boxes are **not** drawn by default; a box (in the target's tracking colour)
  appears only for people the operator right-clicked to track.

**Grid:** 2×2, 8px gap, each tile bordered (`border2`), `bg0`.

**Tile overlay chrome** (drawn in `BoxOverlay::paintEvent`, on top of video):
- top-left: blinking `accent` dot (7px) + camera name (mono, white, shadow) e.g. `CH1 · LOBBY EAST`
- top-right: resolution/codec caption (mono, dim white) e.g. `2592×1520 · 30FPS · H.265`
- bottom-left: **OCC badge** (`OCC <n>/<cap>`, occ number colored by load) + **flow badge** (`+3.2/min`)
- bottom-right: timestamp (mono) — reuse the existing delay/ts HUD, restyled
- detection boxes: **1.5px `accent` border**, tag label above-left with mono text
  `P-<id> · <ATTR>` on an accent background
- Optional flourishes: faint scanline (`repeating-linear-gradient`) + inner vignette — can be
  approximated with a subtle `QLinearGradient`/overlay or skipped in v1.

**Bottom strip (172px, 2 panels):**
- **EVENT TIMELINE** (flex 1.6): scrolling rows `time | severity pill | source | message`.
  Severity colors: INFO `accent`, WARN `amber`, FIRE `alarm`. *Wire to real data:* stream
  up/down, reconnects, DB status. Mock the rest for now.
- **ZONE OCCUPANCY · FORECAST +5 MIN** (flex 1): per-channel horizontal bar + a prediction
  tick mark + `now → pred` text. *Wire occupancy to real detection counts* from
  `DetectionFeed`; prediction can be a placeholder tick until `/prediction` is wired.

### 3.4 Placeholder page (`PlaceholderPage` : QWidget)
Reusable: dark bg, centered screen title + "화면 준비 중 / pending backend data", styled with
the same panel chrome so navigation feels complete.

### 3.5 Tracking panel (`TrackingPanel` : QWidget, 400px — inside LIVE)
Sits to the right of the 2×2 grid inside `LiveViewer`, always visible (the wall is a
fixed 2x2 grid since 08-19). Top→bottom:
- **Header:** `TRACKING · 동선` + `floor map · sector B` + blinking green `LIVE`.
- **Selected strip (38px):** `P-<id>` accent mono + `TRACKING` pill + `dwell mm:ss` +
  `now CH<n>`.
- **FLOOR MAP · 2×2:** `FloorMiniMap`, one cell per channel in the same order as the
  LIVE grid (`row = ch/2, col = ch%2`). Current channel = `accent` border + 8% tint;
  visited = `border2`. Path = dashed 2px `accent` polyline, 4px grey waypoints, the
  last point 7px `accent` with a `textHi` ring and a `NOW · HH:mm:ss` label.
  Point position = box bottom-centre (the person's feet), mapped into the inner 80% of
  the cell. No fan projection — that's `CrowdPage::FloorCanvas`'s job if a surveyed
  floor plan ever arrives.
- **Active (104px, caption amended 08-19 - the click rules moved into a tooltip):** one clickable `TrackRow` per live target
  (`dot | P-<id> | CH<n> | zone | last-seen`), already-tracked rows sorted to the top.
  The panel emits `selection_changed(QVector<TrackId>)` and `LiveViewer::apply_selection`
  highlights each target **in its own channel only**.
  *This list is not in the mockup* — it exists because until `global_id` is published the
  panel has no other way to name a target (§Data below).
- **PATH LOG:** one row per **channel change**, all selected targets interleaved newest
  first (`time | dot | P-<id> | CH<n> | zone`; the id column appears only when >1 target).
  Per-sample rows would just scroll at a 200ms poll.

**Multi-target rules** (same for a row click and a right-click on a tile box; amended 08-19)
- Every click **toggles**: not tracked -> added to the set (keeping the others),
  already tracked -> released (its box disappears - there is no default box anymore).
  No modifier keys; Ctrl+click was removed.
- **Nothing is selected at startup, and the empty set is a legal state.** The panel never
  picks a target on its own: highlighting somebody the operator did not choose would make
  the screen lie about who is under surveillance.
- Up to `Theme::track_color_count()` (6) at once; adding past the cap drops the oldest
  rather than silently ignoring the click.
- Selection order fixes the colour: `Theme::track_color(slot)`. Slot 0 is **`amber`** —
  the same highlight the single-selection build used. `accent` blue is excluded because it
  is the default colour of an *un*selected detection box (a blue "selected" box reads as
  not-selected); `alarm` red is excluded because it means fire.
- The same colour is used by the floor-map polyline, the PATH LOG dot/id, the ACTIVE row,
  and the 2.5px box border in `BoxOverlay`, so "that line is that person" is readable.
  `ChannelView` therefore holds `QHash<int, QColor>`, not a single selected id.
- `refresh()` re-emits `selection_changed` every tick (not only on change) — a target that
  walks into another camera changes which channel/`object_id` must be highlighted.
  `set_selected_objects` early-returns on an equal hash, so there is no extra repaint.

**Data (`TrackHistory` singleton, `track_history.{h,cpp}`)**
- Keyed by `TrackId`: `global_id` when non-zero, else `(channel, object_id)`.
  `global_id` is the real cross-camera re-ID key but is not published yet, so today
  every path is single-channel and the floor map draws inside one cell. When the field
  starts arriving (`detection_feed.cpp` already parses it) the key switches by itself
  and cross-channel paths appear — `TrackingPanel` needs no change.
- Fed from `ChannelView::on_boxes_updated`, **Human (`category == 1`) only** — Face/Head
  are parts of the same person and would triple the path.
- Retention 10 min / 240 points per target; a point is kept only if it moved ≥1.5% of the
  frame, ≥600ms passed, or the channel changed. `active()` = seen within 3s of the
  camera clock. `updated()` is coalesced to 250ms so the panel doesn't rebuild per box.

**Not portable from the mockup:** the `conf 0.91` figure and the PATH LOG confidence
column are dropped — `DetectionBox` carries no confidence and neither does the camera
response, and inventing one on a surveillance screen is worse than omitting it.
Sub-zone names (`zone C1`) are dropped too; `Theme::channel_name` only knows channel
locations, so the zone column shows `LOBBY EAST`.

---

## 4. Current Qt structure → target

### Current (what exists today)
```
main.cpp            QApplication + MainWindow
mainwindow.{h,cpp}  QListWidget sidebar (180px) + QStackedWidget[ LiveViewer ]
live_viewer.{h,cpp} QGridLayout 2×2 of ChannelView; stream_url(); fullscreen toggle;
                    keyPressEvent(+/- playback delay); owns DetectionFeed
channel_view.{h,cpp}QWidget = [ backend video widget ] + [ BoxOverlay (transparent,
                    QPainter draws detection boxes + status text + delay HUD) ]
video_backend + gstreamer_backend / ffmpeg_backend / qmediaplayer_backend  (unchanged)
detection_feed / box_source   HTTP poll → per-channel detections (occupancy source)
```

### Target (after this task)
```
theme.{h,cpp}        NEW — color constants (QColor) + global QSS + font setup + occColor()
top_bar.{h,cpp}      NEW — TopBar widget
nav_rail.{h,cpp}     NEW — NavRail widget (replaces the QListWidget sidebar)
placeholder_page.h   NEW — PlaceholderPage widget
mainwindow.{h,cpp}   REWRITE shell: TopBar (top) + [ NavRail | QStackedWidget ];
                     stack = LiveViewer + 5×PlaceholderPage
live_viewer.{h,cpp}  ADD header bar + bottom strip; dark bg; keep grid/streaming logic
channel_view.cpp     RESKIN BoxOverlay::paintEvent (new tile chrome + box style)
main.cpp             apply Theme::stylesheet() + load fonts before w.show()
CMakeLists.txt       add the new sources
```

### HTML → Qt mapping
| HTML construct | Qt equivalent |
|---|---|
| global `<style>` colors/fonts | `QApplication::setStyleSheet(Theme::stylesheet())` |
| flexbox rows/cols | `QHBoxLayout` / `QVBoxLayout` / `QGridLayout` |
| `sc-for` lists | build widgets in a loop |
| `sc-if` screens | `QStackedWidget` pages |
| inline `style=`, `style-hover=` | per-widget `setStyleSheet` + `:hover` in QSS |
| toggle switches | small custom `QAbstractButton` or a styled `QCheckBox` |
| status dots / blink / pulse | `QPainter` + `QTimer`/`QPropertyAnimation` |
| tile OSD text over video | `BoxOverlay::paintEvent` (already the pattern) |
| SVG sparklines / heat | later screens — skip for this task |

---

## 5. Implementation order (start here)

1. **`theme.{h,cpp}`** — palette as `QColor` constants, `QString stylesheet()`, `occColor(load)`,
   font-family constants. Apply in `main.cpp`. Verify the app just looks dark + correct fonts.
2. **`nav_rail` + `top_bar`** — build the two chrome widgets standalone; check visually.
3. **`mainwindow` rewrite** — TopBar on top, NavRail + QStackedWidget below; wire
   `NavRail::screenSelected` → `QStackedWidget::setCurrentIndex`. Add 5 `PlaceholderPage`s.
4. **`live_viewer`** — add the header bar and the bottom strip; dark background; keep all
   existing streaming/fullscreen/delay logic intact.
5. **`channel_view` (`BoxOverlay`)** — reskin the overlay: LIVE dot, cam name, resolution
   caption, OCC/flow badges, timestamp, new detection-box style.
6. **Wire real data** into the strip where cheap: occupancy bars ← `DetectionFeed` counts;
   event timeline ← stream up/down + reconnect + DB status. Mock the rest.

**Build:** `cmake --build <build> --target gstream_VMS` (Qt 6.11 MinGW; see existing
CMakeLists / LAN_TEST_CHECKLIST for the exact env). Test-run per
[LAN_TEST_CHECKLIST.md](LAN_TEST_CHECKLIST.md).

---

## 6. Constraints & notes for the implementer

- **Don't break the video pipeline.** The backends (`gstreamer_backend`/`ffmpeg_backend`) and
  the `ChannelView` video-widget hosting must stay as-is; only the **overlay drawing** and the
  **surrounding chrome** change. The video widget fills the tile; `BoxOverlay` is a transparent
  sibling on top (mouse-transparent). Keep that structure.
- **`data-screen-label`** attributes in the HTML name each screen ("Live Monitoring", "Crowd
  Analytics", …) — use them as the page titles.
- **Real numbers to prefer over mock:** camera is **2592×1520** native but the streams are
  profile-based (profile6 = 1920×1080 currently). The HTML's "H.265" caption is aspirational —
  the streams are **H.264**; use the real codec/resolution from the running profile if easy.
- **Occupancy/flow** come from `DetectionFeed` (per-channel detection counts) — the design's
  `OCC n/cap` maps to live detection count vs a per-channel capacity constant.
- **Config already exists** for backend/transport/decoder in `QSettings("GuardX","VMS")` — the
  UI doesn't need to touch it, but a settings surface could live under ZONES later.
- **Scope discipline:** the 5 non-Live screens are **placeholders only** this pass. Their full
  HTML (heatmap, tracking, reports, devices, zones, disaster overlay) is preserved in the
  design file for when the backend data exists.

---

## 7. Reference

- Design mockup: [`GuardX VMS.dc.html`](GuardX%20VMS.dc.html) (full source; §render logic at
  the bottom has exact color thresholds and mock data shapes)
- Architecture / backends: [ARCHITECTURE.md](ARCHITECTURE.md)
- Today's work context: [WORKLOG_2026-07-23.md](WORKLOG_2026-07-23.md)
- Build/run: [LAN_TEST_CHECKLIST.md](LAN_TEST_CHECKLIST.md)
