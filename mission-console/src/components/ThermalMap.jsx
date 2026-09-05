import { useEffect, useRef } from 'react';
import { HOTSPOT_THRESHOLD_C, THERMAL_H, THERMAL_W } from '../data/telemetry.js';

/* ---------------------------------------------------------------------------
   MLX90640 frame renderer.

   Written into a 32x24 ImageData buffer and scaled by CSS with
   `image-rendering: pixelated`, rather than mounting 768 DOM nodes and
   re-reconciling them twice a second. The backing store stays at true sensor
   resolution, so no interpolation is invented between pixels.

   Ramp is monochrome — white (cold) to black (hot) — with the accent reserved
   for pixels over the hotspot threshold, so a breach is the only colour in the
   frame and cannot be confused with a merely warm region.
   --------------------------------------------------------------------------- */

const ACCENT = [204, 34, 0];

/** Legend bins, matching the 240 -> 20 ramp used per pixel. */
const RAMP_STEPS = [240, 208, 176, 144, 112, 80, 48, 20];

export function ThermalMap({ reading }) {
  const canvasRef = useRef(null);
  const frame = reading?.thermal?.frame;
  const lo = reading?.thermal?.min_c ?? 20;
  const hi = Math.max(reading?.thermal?.max_c ?? 40, lo + 6);
  const hotspots = reading?.thermal?.hotspot_count ?? 0;

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const ctx = canvas.getContext('2d', { alpha: false });
    if (!ctx) return;

    const img = ctx.createImageData(THERMAL_W, THERMAL_H);
    const span = Math.max(hi - lo, 0.001);

    for (let i = 0; i < THERMAL_W * THERMAL_H; i++) {
      const c = frame ? frame[i] : lo;
      const o = i * 4;
      if (c > HOTSPOT_THRESHOLD_C) {
        [img.data[o], img.data[o + 1], img.data[o + 2]] = ACCENT;
      } else {
        // 240 (cold) -> 20 (hot); stops short of pure black so the accent reads
        // as distinct from the hottest sub-threshold pixels.
        const v = Math.round(240 - Math.min(1, Math.max(0, (c - lo) / span)) * 220);
        img.data[o] = img.data[o + 1] = img.data[o + 2] = v;
      }
      img.data[o + 3] = 255;
    }
    ctx.putImageData(img, 0, 0);
  }, [frame, lo, hi]);

  return (
    <>
      <canvas
        ref={canvasRef}
        className="thermal__canvas"
        width={THERMAL_W}
        height={THERMAL_H}
        role="img"
        aria-label={`Thermal frame, ${THERMAL_W} by ${THERMAL_H} pixels. Range ${lo.toFixed(
          1
        )} to ${hi.toFixed(1)} degrees Celsius. ${
          hotspots ? `${hotspots} pixels above the hotspot threshold.` : 'No pixels above the hotspot threshold.'
        }`}
      />
      {/* Discrete bins, not a continuous ramp: quantised steps are easier to
          match back to a pixel, and keep the sheet gradient-free. */}
      <div className="thermal__scale" aria-hidden="true">
        <span>{lo.toFixed(1)}&deg;C</span>
        <span className="thermal__ramp">
          {RAMP_STEPS.map((v) => (
            <i key={v} style={{ background: `rgb(${v},${v},${v})` }} />
          ))}
          <i style={{ background: 'var(--accent)' }} />
        </span>
        <span>&gt;{HOTSPOT_THRESHOLD_C}&deg;C</span>
      </div>
    </>
  );
}
