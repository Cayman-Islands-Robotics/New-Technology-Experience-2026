import { useEffect, useRef } from 'react';
import { HOTSPOT_THRESHOLD_C, THERMAL_H, THERMAL_W } from '../data/telemetry.js';

/* ---------------------------------------------------------------------------
   MLX90640 hotspot map.

   The firmware publishes the coordinates of pixels over the hotspot threshold
   (`thermal.hotspot_px`), not the raw frame, so this plots exactly those on a
   blank 32x24 field. Written into a 32x24 ImageData buffer and scaled by CSS
   with `image-rendering: pixelated`, so each sensor pixel stays a hard square.
   --------------------------------------------------------------------------- */

const ACCENT = [204, 34, 0];

export function ThermalMap({ reading }) {
  const canvasRef = useRef(null);
  const hotspotPx = reading?.thermal?.hotspot_px;
  const hotspots = reading?.thermal?.hotspot_count ?? 0;

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const ctx = canvas.getContext('2d', { alpha: false });
    if (!ctx) return;

    const img = ctx.createImageData(THERMAL_W, THERMAL_H);

    // The firmware publishes hotspot coordinates, not the raw 768-pixel frame,
    // so the canvas shows the flagged pixels on a blank field.
    for (let i = 0; i < THERMAL_W * THERMAL_H; i++) {
      const o = i * 4;
      img.data[o] = img.data[o + 1] = img.data[o + 2] = 240;
      img.data[o + 3] = 255;
    }
    for (const [x, y] of hotspotPx ?? []) {
      if (x < 0 || y < 0 || x >= THERMAL_W || y >= THERMAL_H) continue;
      const o = (y * THERMAL_W + x) * 4;
      [img.data[o], img.data[o + 1], img.data[o + 2]] = ACCENT;
    }
    ctx.putImageData(img, 0, 0);
  }, [hotspotPx]);

  return (
    <>
      <canvas
        ref={canvasRef}
        className="thermal__canvas"
        width={THERMAL_W}
        height={THERMAL_H}
        role="img"
        aria-label={`Thermal hotspot map, ${THERMAL_W} by ${THERMAL_H} pixels. ${
          hotspots ? `${hotspots} pixels above the hotspot threshold.` : 'No pixels above the hotspot threshold.'
        }`}
      />
      <div className="thermal__scale" aria-hidden="true">
        <span className="thermal__ramp">
          <i style={{ background: 'rgb(240,240,240)' }} />
        </span>
        <span>below {HOTSPOT_THRESHOLD_C}&deg;C</span>
        <span className="thermal__ramp">
          <i style={{ background: 'var(--accent)' }} />
        </span>
        <span>&gt;{HOTSPOT_THRESHOLD_C}&deg;C</span>
      </div>
    </>
  );
}
