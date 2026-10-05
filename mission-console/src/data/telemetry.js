/* ---------------------------------------------------------------------------
   Reading documents from the Firestore `readings` collection.

   `pi_sensor_thermal_rover.py` writes the Arduino's JSON line plus lat, lon,
   image_url and server_time. Fields the firmware does not publish (or has not
   published yet — e.g. no GPS fix) come through as null; nothing here invents
   a value to fill the gap.
   --------------------------------------------------------------------------- */

export const THERMAL_W = 32;
export const THERMAL_H = 24;

/** Firmware constant mirrored from scout_sensor_hub.ino. */
export const HOTSPOT_THRESHOLD_C = 38;

const numOrNull = (v) => (typeof v === 'number' && Number.isFinite(v) ? v : null);

/** Temperatures are published to 0.1 °C; this drops float noise such as 25.200001. */
const tempOrNull = (v) => (numOrNull(v) == null ? null : Math.round(v * 10) / 10);

/** Accepts [[x, y], ...] (firmware) or [{x, y}, ...] (Firestore-safe form). */
function hotspotPairs(px) {
  if (!Array.isArray(px)) return [];
  return px
    .map((p) => (Array.isArray(p) ? p : p && typeof p === 'object' ? [p.x, p.y] : null))
    .filter((p) => p && Number.isFinite(p[0]) && Number.isFinite(p[1]));
}

/**
 * The firmware publishes its frame size as thermal.width / thermal.height.
 * A size other than 32x24, or a hotspot outside that field, is only logged —
 * the reading is still rendered.
 */
function checkThermalDimensions(seq, th, px) {
  if (th.width != null && th.height != null && (th.width !== THERMAL_W || th.height !== THERMAL_H)) {
    console.log(`[thermal] seq ${seq ?? '–'}: got ${th.width}x${th.height}, expected ${THERMAL_W}x${THERMAL_H}`);
  }
  for (const [x, y] of px) {
    if (x < 0 || x >= THERMAL_W || y < 0 || y >= THERMAL_H) {
      console.log(`[thermal] seq ${seq ?? '–'}: hotspot pixel (${x}, ${y}) is outside ${THERMAL_W}x${THERMAL_H}`);
    }
  }
}

/**
 * thermal.pixels is a flat, row-by-row grid of temperatures in °C. The firmware
 * names its size in thermal.pixels_width / pixels_height (16x16, the sensor
 * frame averaged down); a 768-value array without them is a full 32x24 frame.
 * Returns null when there is no grid or its size cannot be told.
 */
function thermalGrid(th) {
  if (!Array.isArray(th.pixels) || th.pixels.length === 0) return null;
  let width = th.pixels_width;
  let height = th.pixels_height;
  if (!Number.isInteger(width) || !Number.isInteger(height) || width * height !== th.pixels.length) {
    if (th.pixels.length !== THERMAL_W * THERMAL_H) return null;
    width = THERMAL_W;
    height = THERMAL_H;
  }
  return { width, height, values: th.pixels.map(numOrNull) };
}

/** Firestore snapshot -> the plain reading shape the views render. */
export function fromSnapshot(snap) {
  const d = snap.data({ serverTimestamps: 'estimate' });
  const gas = d.gas_ppm ?? {};
  const th = d.thermal ?? {};
  checkThermalDimensions(d.seq, th, hotspotPairs(th.hotspot_px));
  return {
    id: snap.id,
    seq: numOrNull(d.seq),
    server_time: d.server_time?.toMillis?.() ?? null,
    lat: numOrNull(d.lat),
    lon: numOrNull(d.lon),
    gas_ppm: {
      mq2_smoke: numOrNull(gas.mq2_smoke),
      mq4_methane: numOrNull(gas.mq4_methane),
      mq7_co: numOrNull(gas.mq7_co),
      mq135_voc: numOrNull(gas.mq135_voc),
    },
    thermal: {
      min_c: tempOrNull(th.min_c),
      avg_c: tempOrNull(th.avg_c),
      max_c: tempOrNull(th.max_c),
      hotspot_count: numOrNull(th.hotspot_count),
      hotspot_px: hotspotPairs(th.hotspot_px),
      grid: thermalGrid(th),
    },
    marl_pct: numOrNull(d.marl_pct),
    image_url: typeof d.image_url === 'string' ? d.image_url : null,
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
