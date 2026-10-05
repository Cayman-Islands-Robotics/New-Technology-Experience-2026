# SCOUT-01 Console

Operator console for the thermal rover: mission **schedule**, subsystem
**status**, and **live telemetry**. Telemetry comes only from the Firestore
`readings` collection in the `thermal-rover` project — there is no simulated
data. Until the collection has a document, the Status and Live tabs say so.

```
cp .env.example .env.local   # Firebase web config
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
- **System fonts only.** No webfont requests. The only runtime network traffic
  is the Firestore listener and the latest reading's `image_url`. Numeric cells use `font-variant-numeric: tabular-nums` so values
  do not reflow as they update.
- **Three runtime dependencies:** `react`, `react-dom`, `firebase`. The chart is raw SVG and
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
  firebase.js             Firestore init from .env.local
  useTelemetry.js         the single seam between UI and data source
  lib/format.js           pure formatting helpers, React-free
  data/
    telemetry.js          Firestore doc -> reading + hazard rules (no presentation)
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

The firmware's temperature grid (`thermal.pixels`, 16×16 — the 32×24 sensor
frame averaged down) is coloured yellow → orange → red across that frame's own
range (at least 5 °C wide, so a uniform room is not stretched into noise),
written into an `ImageData` buffer at grid resolution and scaled by CSS with
`image-rendering: pixelated`, rather than mounting hundreds of DOM nodes and
reconciling them on every reading. No interpolation is invented between cells.
A reading with no grid falls back to plotting `thermal.hotspot_px` on a blank
32×24 field. Server-rendered
markup for the live view dropped from ~41 kB to ~14 kB as a result.

## Data source

`src/firebase.js` initialises Firestore from `.env.local`. `useTelemetry()`
holds an `onSnapshot` listener on
`query(collection(db,'readings'), orderBy('server_time','desc'), limit(90))`
and is the only module that knows where readings come from.
`data/telemetry.js#fromSnapshot` turns each document into the shape the views
render; any field the document lacks becomes `null` and displays as `—`.

The firmware publishes `thermal.max_c`, `avg_c`, `hotspot_count`, `hotspot_px`,
the temperature grid `thermal.pixels` (with `pixels_width` / `pixels_height`)
and the four `gas_ppm` channels; the Pi adds `lat`, `lon`, `image_url` and
`server_time`. It does not publish `thermal.min_c`, `marl_pct`, battery, or
link stats, so the console does not show them.

Firestore rejects arrays nested directly in arrays, so the firmware's
`hotspot_px: [[x, y], ...]` will fail to write once a hotspot is present.
`fromSnapshot` already accepts `[{x, y}, ...]`, so converting it on the Pi
before `publish_reading` is enough.
