import { useEffect, useRef } from 'react';
import { HOTSPOT_THRESHOLD_C, THERMAL_H, THERMAL_W } from '../data/telemetry.js';

/* ---------------------------------------------------------------------------
   MLX90640 thermal map.

   When the reading carries a temperature grid (`thermal.grid`, from the
   firmware's `thermal.pixels`), every cell is coloured on a yellow -> orange ->
   red ramp stretched over that frame's own range. Readings without a grid fall
   back to plotting the hotspot coordinates (`thermal.hotspot_px`) on a blank
   32x24 field. Either way the image is written into an ImageData buffer at
   grid resolution and scaled by CSS with `image-rendering: pixelated`, so each
   cell stays a hard square.
   --------------------------------------------------------------------------- */

const ACCENT = [204, 34, 0];
const BLANK = [240, 240, 240];

/* Coolest -> hottest. The hot end is the console's accent red. */
const RAMP = [
  [255, 221, 51], // yellow
  [255, 136, 0], // orange
  ACCENT, // red
];

/* A frame of a room at one temperature is all sensor noise; without a floor on
   the range that noise would be stretched across the whole ramp. */
const MIN_SPAN_C = 5;

const LEGEND_STEPS = 7;

/** t in 0..1 -> [r, g, b] along RAMP. */
function rampColor(t) {
  const pos = Math.min(1, Math.max(0, t)) * (RAMP.length - 1);
  const i = Math.min(RAMP.length - 2, Math.floor(pos));
  const f = pos - i;
  return RAMP[i].map((c, k) => Math.round(c + (RAMP[i + 1][k] - c) * f));
}

/** Lowest and highest temperature the ramp spans for this grid, or null if it has no values. */
function gridRange(grid) {
  let lo = Infinity;
  let hi = -Infinity;
  for (const v of grid.values) {
    if (v == null) continue;
    if (v < lo) lo = v;
    if (v > hi) hi = v;
  }
  if (lo === Infinity) return null;
  return { lo, hi: Math.max(hi, lo + MIN_SPAN_C) };
}

export function ThermalMap({ reading }) {
  const canvasRef = useRef(null);
  const grid = reading?.thermal?.grid ?? null;
  const hotspotPx = reading?.thermal?.hotspot_px;
  const hotspots = reading?.thermal?.hotspot_count ?? 0;
  const range = grid ? gridRange(grid) : null;
  const width = grid?.width ?? THERMAL_W;
  const height = grid?.height ?? THERMAL_H;

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const ctx = canvas.getContext('2d', { alpha: false });
    if (!ctx) return;

    const img = ctx.createImageData(width, height);
    const paint = (i, [r, g, b]) => {
      const o = i * 4;
      img.data[o] = r;
      img.data[o + 1] = g;
      img.data[o + 2] = b;
      img.data[o + 3] = 255;
    };

    if (grid && range) {
      const span = range.hi - range.lo;
      grid.values.forEach((v, i) => paint(i, v == null ? BLANK : rampColor((v - range.lo) / span)));
    } else {
      // No temperature grid in this reading: show the flagged pixels on a blank field.
      for (let i = 0; i < width * height; i++) paint(i, BLANK);
      for (const [x, y] of hotspotPx ?? []) {
        if (x < 0 || y < 0 || x >= width || y >= height) continue;
        paint(y * width + x, ACCENT);
      }
    }
    ctx.putImageData(img, 0, 0);
  }, [grid, range?.lo, range?.hi, hotspotPx, width, height]);

  const hotspotText = hotspots
    ? `${hotspots} pixels above the hotspot threshold.`
    : 'No pixels above the hotspot threshold.';

  return (
    <>
      <canvas
        ref={canvasRef}
        className="thermal__canvas"
        width={width}
        height={height}
        role="img"
        aria-label={
          range
            ? `Thermal map, ${width} by ${height} cells, ${range.lo.toFixed(1)} to ${range.hi.toFixed(1)} degrees Celsius, yellow coolest to red hottest. ${hotspotText}`
            : `Thermal hotspot map, ${width} by ${height} pixels. ${hotspotText}`
        }
      />
      {range ? (
        <div className="thermal__scale" aria-hidden="true">
          <span>{range.lo.toFixed(1)}&deg;C</span>
          <span className="thermal__ramp">
            {Array.from({ length: LEGEND_STEPS }, (_, k) => (
              <i key={k} style={{ background: `rgb(${rampColor(k / (LEGEND_STEPS - 1)).join(',')})` }} />
            ))}
          </span>
          <span>{range.hi.toFixed(1)}&deg;C</span>
        </div>
      ) : (
        <div className="thermal__scale" aria-hidden="true">
          <span className="thermal__ramp">
            <i style={{ background: `rgb(${BLANK.join(',')})` }} />
          </span>
          <span>below {HOTSPOT_THRESHOLD_C}&deg;C</span>
          <span className="thermal__ramp">
            <i style={{ background: 'var(--accent)' }} />
          </span>
          <span>&gt;{HOTSPOT_THRESHOLD_C}&deg;C</span>
        </div>
      )}
    </>
  );
}
