/* ---------------------------------------------------------------------------
   Mock telemetry source.

   Emits readings in exactly the shape `pi_sensor_thermal_rover.py` writes to
   the `readings` Firestore collection, so swapping this module for a real
   onSnapshot listener later is a drop-in change — nothing downstream of
   `useTelemetry` knows where the readings came from.
   --------------------------------------------------------------------------- */

export const THERMAL_W = 32;
export const THERMAL_H = 24;

/** Site centre — George Town landfill, Grand Cayman (same default as the dashboard). */
export const SITE = { lat: 19.3, lon: -81.25 };

/** Firmware constants mirrored from scout_sensor_hub.ino. */
export const HOTSPOT_THRESHOLD_C = 38;
export const WARMUP_MS = 180000; // 3 minute gas-sensor warm-up
export const TELEMETRY_INTERVAL_MS = 2000;

const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
const round1 = (v) => Math.round(v * 10) / 10;

/* --- ambient state the walker mutates ------------------------------------ */
function initialState() {
  return {
    seq: 1420,
    ambientC: 29.5,
    smoke: 62,
    methane: 14,
    co: 9,
    voc: 168,
    lat: SITE.lat,
    lon: SITE.lon,
    heading: Math.random() * Math.PI * 2,
    marl: 41,
    // Two drifting subsurface hotspots, in thermal-pixel coordinates.
    plumes: [
      { x: 11, y: 9, r: 4.2, intensity: 6, vx: 0.09, vy: 0.05 },
      { x: 23, y: 15, r: 3.1, intensity: 3, vx: -0.06, vy: 0.08 },
    ],
    flareUntil: 0,
  };
}

/** Random walk one step, with an occasional correlated gas + heat flare-up. */
function step(s) {
  const now = Date.now();
  const flaring = now < s.flareUntil;

  if (!flaring && Math.random() < 0.06) {
    // A pocket of buried waste ignites: gas and temperature move together.
    s.flareUntil = now + 14000 + Math.random() * 12000;
    const p = s.plumes[Math.floor(Math.random() * s.plumes.length)];
    p.intensity = 16 + Math.random() * 9;
    p.r = 5 + Math.random() * 2;
  }

  const drift = (v, target, jitter) => v + (target - v) * 0.12 + (Math.random() - 0.5) * jitter;

  s.smoke = clamp(drift(s.smoke, flaring ? 230 : 62, 14), 20, 420);
  s.co = clamp(drift(s.co, flaring ? 74 : 9, 4), 1, 160);
  s.methane = clamp(drift(s.methane, flaring ? 46 : 14, 5), 2, 120);
  s.voc = clamp(drift(s.voc, flaring ? 340 : 168, 22), 60, 600);
  s.ambientC = clamp(drift(s.ambientC, 29.5, 0.5), 26, 34);

  // Rover creeps along a wandering transect over the cell.
  s.heading += (Math.random() - 0.5) * 0.5;
  s.lat = clamp(s.lat + Math.cos(s.heading) * 0.00016, SITE.lat - 0.004, SITE.lat + 0.004);
  s.lon = clamp(s.lon + Math.sin(s.heading) * 0.00016, SITE.lon - 0.004, SITE.lon + 0.004);

  s.marl = clamp(s.marl + (Math.random() - 0.48) * 2.2, 12, 88);

  for (const p of s.plumes) {
    p.x += p.vx; p.y += p.vy;
    if (p.x < 3 || p.x > THERMAL_W - 3) p.vx *= -1;
    if (p.y < 3 || p.y > THERMAL_H - 3) p.vy *= -1;
    if (!flaring) p.intensity += (5 - p.intensity) * 0.08;
  }

  s.seq += 1;
  return s;
}

/** Render the 32x24 MLX90640 frame implied by the current plume field. */
function renderFrame(s) {
  const frame = new Float32Array(THERMAL_W * THERMAL_H);
  for (let y = 0; y < THERMAL_H; y++) {
    for (let x = 0; x < THERMAL_W; x++) {
      let t = s.ambientC + (Math.random() - 0.5) * 0.7;
      for (const p of s.plumes) {
        const d2 = (x - p.x) ** 2 + (y - p.y) ** 2;
        t += p.intensity * Math.exp(-d2 / (2 * p.r * p.r));
      }
      frame[y * THERMAL_W + x] = t;
    }
  }
  return frame;
}

/** Build one Firestore-shaped reading document from the walker state. */
export function makeReading(s) {
  step(s);
  const frame = renderFrame(s);

  let minC = Infinity, maxC = -Infinity, sum = 0;
  const hotspotPx = [];
  for (let i = 0; i < frame.length; i++) {
    const t = frame[i];
    if (t < minC) minC = t;
    if (t > maxC) maxC = t;
    sum += t;
    if (t > HOTSPOT_THRESHOLD_C && hotspotPx.length < 10) {
      hotspotPx.push([i % THERMAL_W, Math.floor(i / THERMAL_W)]);
    }
  }

  return {
    seq: s.seq,
    server_time: Date.now(),
    lat: s.lat,
    lon: s.lon,
    gas_ppm: {
      mq2_smoke: Math.round(s.smoke),
      mq4_methane: Math.round(s.methane),
      mq7_co: Math.round(s.co),
      mq135_voc: Math.round(s.voc),
    },
    thermal: {
      min_c: round1(minC),
      avg_c: round1(sum / frame.length),
      max_c: round1(maxC),
      hotspot_count: hotspotPx.length,
      hotspot_px: hotspotPx,
      frame, // extra: not published by the firmware, used for the local heatmap
    },
    marl_pct: Math.round(s.marl),
    image_url: null,
  };
}

/* --- hazard classification (same thresholds as the field dashboard) ------- */
export function hazardLevel(r) {
  if (!r) return 'idle';
  const hot = (r.thermal?.max_c || 0) > HOTSPOT_THRESHOLD_C;
  const gas = (r.gas_ppm?.mq2_smoke || 0) > 150 || (r.gas_ppm?.mq7_co || 0) > 50;
  if (hot && gas) return 'ember'; // correlated hazard
  if (hot || gas) return 'amber'; // single-sensor flag
  return 'safe';
}

/** Seed a short backlog so the console never opens on an empty chart. */
export function seedHistory(n = 40) {
  const s = initialState();
  const out = [];
  const t0 = Date.now() - n * TELEMETRY_INTERVAL_MS;
  for (let i = 0; i < n; i++) {
    const r = makeReading(s);
    r.server_time = t0 + i * TELEMETRY_INTERVAL_MS;
    delete r.thermal.frame; // only the newest frame is worth keeping in memory
    out.push(r);
  }
  return { state: s, history: out };
}

export { initialState };
