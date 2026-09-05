# SCOUT-01 Console

Operator console for the thermal rover: mission **schedule**, subsystem
**status**, and **live telemetry**. Runs entirely on a simulated reading
stream — no Firebase, Pi, or Arduino required to exercise every path.

```
npm install
npm run dev       # http://localhost:5173
npm run build     # static bundle -> dist/
npm run preview   # serve dist/
```

## Design constraints

The interface is deliberately utilitarian. These are enforced, not aspirational:

- **No gradients, glassmorphism, glows, or drop shadows.** Audited in CI-able
  form: `grep -rniE "gradient|box-shadow|backdrop-filter|blur\(" src/` returns
  only comments.
- **Monochrome plus one functional accent.** `--accent` (`#cc2200`) means
  exactly one thing — a value past its alarm threshold. It is never used for
  selection, branding, or emphasis. Selection is expressed by inversion
  (black fill, white text), which keeps hue load at one.
- **No motion.** `base.css` carries a global `transition: none !important;
  animation: none !important` guard so motion cannot be reintroduced by a later
  edit without a deliberate override. State changes are instantaneous.
- **`border-radius: 0`** everywhere, via a single `--radius` token.
- **System fonts only.** No webfont requests; no network requests of any kind
  at runtime. Numeric cells use `font-variant-numeric: tabular-nums` so values
  do not reflow as they update.
- **Two runtime dependencies:** `react`, `react-dom`. The chart is raw SVG and
  the thermal map is a raw `<canvas>` — no charting or component library.

State is legible without colour: subsystem and block states render as bracketed
monospace codes (`[NOMINAL]`, `[CAUTION]`, `[HAZARD]`), so the console survives
monochrome print and colour-blind operators.

## Accessibility

- Full WAI-ARIA tab pattern with a roving tabindex — `Left`/`Right`/`Home`/`End`
  move selection, and focus moves with it. Inactive panels unmount rather than
  hide, keeping them out of the tab order and the a11y tree.
- Skip link to `<main>`; one `<main>` landmark; every `<section>` labelled by
  its own heading.
- Tables are real tables with a visually hidden `<caption>` and `scope="col"`
  headers. Times are `<time datetime>`. Meters expose `role="meter"` with
  explicit bounds.
- The chart carries a one-sentence `aria-label` summary (range and latest value
  per series) — more useful non-visually than sixty read-out points. The
  thermal canvas describes its range and hotspot count.
- The app bar's hazard state is an `aria-live="polite"` region; the 2 s readout
  grid is explicitly `aria-live="off"` so routine updates are not announced.

## Architecture

```
src/
  App.jsx                 shell: app bar, tab nav, panel routing
  useTelemetry.js         the single seam between UI and data source
  lib/format.js           pure formatting helpers, React-free
  data/
    telemetry.js          reading simulator + hazard rules (no presentation)
    schedule.js           five-day plan fixture
    subsystems.js         subsystem roster; each row reads the live document
  components/             AppBar, TabNav, Panel, DataTable, Readout, Meter,
                          DefList, Tag, TrendChart, ThermalMap
  views/                  StatusView, ScheduleView, LiveView
  styles/
    tokens.css            palette, type, 4px spacing scale, geometry
    base.css              reset, semantic defaults, motion guard, a11y utils
    layout.css            app shell and grids
    components.css        component rules
```

Colour never lives in `data/` — the simulator emits numbers, components decide
how to render them.

### Thermal map

The MLX90640 frame is written into a 32×24 `ImageData` buffer and scaled by CSS
with `image-rendering: pixelated`, rather than mounting 768 DOM nodes and
reconciling them twice a second. The backing store stays at true sensor
resolution, so no interpolation is invented between pixels. Server-rendered
markup for the live view dropped from ~41 kB to ~14 kB as a result.

## Wiring to the real backend

`useTelemetry()` is the only module that knows where readings come from.
`data/telemetry.js` emits documents in exactly the shape
`pi_sensor_thermal_rover.py` writes to the `readings` collection:

```js
{ seq, server_time, lat, lon,
  gas_ppm: { mq2_smoke, mq4_methane, mq7_co, mq135_voc },
  thermal: { min_c, avg_c, max_c, hotspot_count, hotspot_px },
  marl_pct, image_url }
```

Going live means replacing the `setInterval` in `useTelemetry` with a Firestore
`onSnapshot` over
`query(collection(db,'readings'), orderBy('server_time','desc'), limit(1))`
and pushing each document through the same `setCurrent` / `setHistory` calls.
Nothing downstream changes.

`thermal.frame` is the one field the simulator adds that the firmware does not
publish — the raw 768-value array behind the heat map. Against a real backend
that panel either drops out or requires a new firmware field; `hotspot_px` is
already published and drives everything else.
