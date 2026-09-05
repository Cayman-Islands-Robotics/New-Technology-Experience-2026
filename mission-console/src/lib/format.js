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
