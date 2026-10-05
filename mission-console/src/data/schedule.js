/* ---------------------------------------------------------------------------
   Mission schedule (static fixture).

   Days are stored as offsets from "today" so the console always opens on a
   live-looking week no matter when it is run. Times are local, 24h "HH:MM".
   --------------------------------------------------------------------------- */

const T = (h, m = 0) => h * 60 + m;

/** Minutes-since-midnight -> "HH:MM". */
export const fmtTime = (mins) =>
  `${String(Math.floor(mins / 60)).padStart(2, '0')}:${String(mins % 60).padStart(2, '0')}`;

export const fmtDur = (mins) =>
  mins >= 60 ? `${Math.floor(mins / 60)}h${mins % 60 ? ` ${mins % 60}m` : ''}` : `${mins}m`;

/** A date object for a given offset from today, at midnight. */
export function dayFor(offset) {
  const d = new Date();
  d.setHours(0, 0, 0, 0);
  d.setDate(d.getDate() + offset);
  return d;
}

/**
 * kind drives the badge colour:
 *   run     — a rover sortie over the landfill cells
 *   ops     — bench work, charging, calibration
 *   review  — data / build reviews with the team
 *   deadline— hard external date, cannot slip
 */
export const SCHEDULE = [
  {
    offset: -1,
    slots: [
      { start: T(7, 30), dur: 45,  kind: 'ops',    title: 'Battery swap + pre-flight', zone: 'Bay 2', crew: 'Mech' },
      { start: T(8, 30), dur: 120, kind: 'run',    title: 'Transect A — north cell', zone: 'Cell A1–A4', crew: 'Nav + Sensors' },
      { start: T(11, 0), dur: 60,  kind: 'ops',    title: 'MLX90640 recalibration', zone: 'Bay 2', crew: 'Sensors' },
      { start: T(14, 0), dur: 90,  kind: 'run',    title: 'Transect B — south berm', zone: 'Cell B2–B5', crew: 'Nav' },
      { start: T(16, 30), dur: 45, kind: 'review', title: 'Daily data review', zone: 'Comms tent', crew: 'All' },
    ],
  },
  {
    offset: 0,
    slots: [
      { start: T(6, 45), dur: 45,  kind: 'ops',    title: 'Gas sensor warm-up', zone: 'Bay 2', crew: 'Sensors' },
      { start: T(7, 30), dur: 60,  kind: 'ops',    title: 'Uplink check + GPS lock', zone: 'Bay 2', crew: 'Comms' },
      { start: T(8, 45), dur: 150, kind: 'run',    title: 'Transect C — active tipping face', zone: 'Cell C1–C6', crew: 'Nav + Sensors' },
      { start: T(11, 30), dur: 45, kind: 'ops',    title: 'Recharge + image offload', zone: 'Bay 2', crew: 'Mech' },
      { start: T(13, 0), dur: 120, kind: 'run',    title: 'Transect D — leachate pond edge', zone: 'Cell D1–D3', crew: 'Nav' },
      { start: T(15, 30), dur: 60, kind: 'review', title: 'Marl classifier retraining', zone: 'Comms tent', crew: 'Software' },
      { start: T(17, 0), dur: 45,  kind: 'review', title: 'Judge Q&A dry run', zone: 'Comms tent', crew: 'All' },
    ],
  },
  {
    offset: 1,
    slots: [
      { start: T(7, 0),  dur: 60,  kind: 'ops',    title: 'Pre-flight + warm-up', zone: 'Bay 2', crew: 'Mech + Sensors' },
      { start: T(8, 30), dur: 180, kind: 'run',    title: 'Full-site sweep', zone: 'Cells A–D', crew: 'Nav + Sensors' },
      { start: T(12, 30), dur: 90, kind: 'review', title: 'Hazard map build', zone: 'Comms tent', crew: 'Software' },
      { start: T(15, 0), dur: 60,  kind: 'ops',    title: 'Spare parts audit', zone: 'Bay 2', crew: 'Mech' },
    ],
  },
  {
    offset: 2,
    slots: [
      { start: T(8, 0),  dur: 120, kind: 'run',    title: 'Verification run', zone: 'Cell C4', crew: 'Nav + Sensors' },
      { start: T(11, 0), dur: 120, kind: 'review', title: 'Report writing block', zone: 'Comms tent', crew: 'All' },
      { start: T(16, 0), dur: 60,  kind: 'deadline', title: 'Submit engineering portfolio', zone: 'Remote', crew: 'All' },
    ],
  },
  {
    offset: 3,
    slots: [
      { start: T(9, 0),  dur: 90,  kind: 'ops',    title: 'Pack-down + transport prep', zone: 'Bay 2', crew: 'Mech' },
      { start: T(13, 0), dur: 60,  kind: 'review', title: 'Presentation rehearsal', zone: 'Comms tent', crew: 'All' },
    ],
  },
];

export const KIND_META = {
  run:      { label: 'Sortie',   tone: 'ember' },
  ops:      { label: 'Ops',      tone: 'thermal' },
  review:   { label: 'Review',   tone: 'safe' },
  deadline: { label: 'Deadline', tone: 'amber' },
};

/** Resolve a slot's status against the current clock. */
export function slotStatus(slot, dayOffset, nowMins) {
  if (dayOffset < 0) return 'done';
  if (dayOffset > 0) return 'upcoming';
  if (nowMins >= slot.start + slot.dur) return 'done';
  if (nowMins >= slot.start) return 'active';
  return 'upcoming';
}
