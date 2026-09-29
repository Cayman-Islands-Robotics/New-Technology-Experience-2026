/* Formatting helpers. Kept free of React so views stay declarative. */

export const clockTime = (ms) =>
  new Date(ms).toLocaleTimeString([], { hour12: false });

/** ISO string for a <time datetime> attribute. */
export const isoAt = (ms) => new Date(ms).toISOString();

export function duration(ms) {
  const s = Math.floor(ms / 1000);
  const pad = (n) => String(n).padStart(2, '0');
  return `${pad(Math.floor(s / 3600))}:${pad(Math.floor((s % 3600) / 60))}:${pad(s % 60)}`;
}

/** Minutes-since-midnight -> "HH:MM". */
export const hhmm = (mins) =>
  `${String(Math.floor(mins / 60)).padStart(2, '0')}:${String(mins % 60).padStart(2, '0')}`;

export const durMins = (mins) =>
  mins >= 60 ? `${Math.floor(mins / 60)}h${mins % 60 ? String(mins % 60).padStart(2, '0') : ''}` : `${mins}m`;

export const pct = (n) => `${Math.round(n)}%`;

/** A reading value with its unit, or an em dash when the document lacks it. */
export const val = (v, unit = '') => (v == null ? '—' : `${v}${unit}`);

/** "lat, lon" to `digits` places, or a no-fix marker. */
export const latLon = (r, digits = 4) =>
  r.lat == null || r.lon == null ? 'no fix' : `${r.lat.toFixed(digits)}, ${r.lon.toFixed(digits)}`;

/** Coarse "how long ago" for a millisecond timestamp. */
export function ago(ms, now) {
  if (ms == null) return '—';
  const s = Math.max(0, Math.round((now - ms) / 1000));
  if (s < 60) return `${s}s ago`;
  if (s < 3600) return `${Math.floor(s / 60)}m ago`;
  if (s < 86400) return `${Math.floor(s / 3600)}h ago`;
  return `${Math.floor(s / 86400)}d ago`;
}
