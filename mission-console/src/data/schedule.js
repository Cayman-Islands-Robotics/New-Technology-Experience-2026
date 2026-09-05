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
      { start: T(7, 30), dur: 45,  kind: 'ops',    title: 'Battery swap + pre-flight', zone: 'Bay 2', crew: 'Mech', desc: 'Two 5000 mAh packs charged and logged. Chassis torque check on the drive pods.' },
      { start: T(8, 30), dur: 120, kind: 'run',    title: 'Transect A — north cell', zone: 'Cell A1–A4', crew: 'Nav + Sensors', desc: 'Baseline sweep with the gas array after a full 3 min warm-up. No hotspots expected on the capped section.' },
      { start: T(11, 0), dur: 60,  kind: 'ops',    title: 'MLX90640 recalibration', zone: 'Bay 2', crew: 'Sensors', desc: 'Emissivity table re-fit against the blackbody reference. Frame rate held at 8 Hz.' },
      { start: T(14, 0), dur: 90,  kind: 'run',    title: 'Transect B — south berm', zone: 'Cell B2–B5', crew: 'Nav', desc: 'Marl coverage imaging pass. Camera set to 15 s stills to keep the cellular uplink under budget.' },
      { start: T(16, 30), dur: 45, kind: 'review', title: 'Daily data review', zone: 'Comms tent', crew: 'All', desc: 'Walk the Firestore log, triage any hazard flags, tag frames for the marl classifier training set.' },
    ],
  },
  {
    offset: 0,
    slots: [
      { start: T(6, 45), dur: 45,  kind: 'ops',    title: 'Gas sensor warm-up', zone: 'Bay 2', crew: 'Sensors', desc: 'MQ-2 / MQ-4 / MQ-7 / MQ-135 heaters on. Firmware holds telemetry for 180 s before the first valid packet.' },
      { start: T(7, 30), dur: 60,  kind: 'ops',    title: 'Uplink check + GPS lock', zone: 'Bay 2', crew: 'Comms', desc: 'SIM7600G-H cellular link verified against Firestore. Wait for a GPGGA fix before rolling.' },
      { start: T(8, 45), dur: 150, kind: 'run',    title: 'Transect C — active tipping face', zone: 'Cell C1–C6', crew: 'Nav + Sensors', desc: 'The high-value pass. Fresh waste on the tipping face is where the correlated smoke + heat signature is most likely to show.' },
      { start: T(11, 30), dur: 45, kind: 'ops',    title: 'Recharge + image offload', zone: 'Bay 2', crew: 'Mech', desc: 'Swap packs, pull the JPEG buffer off the Arduino, confirm Storage upload completed for every capture.' },
      { start: T(13, 0), dur: 120, kind: 'run',    title: 'Transect D — leachate pond edge', zone: 'Cell D1–D3', crew: 'Nav', desc: 'Slow crawl along the pond margin. Methane is the sensor of record here; expect elevated MQ-4 without a thermal signature.' },
      { start: T(15, 30), dur: 60, kind: 'review', title: 'Marl classifier retraining', zone: 'Comms tent', crew: 'Software', desc: 'Feed today’s tagged captures into the Teachable Machine model and compare against the colour-threshold v1 estimate.' },
      { start: T(17, 0), dur: 45,  kind: 'review', title: 'Judge Q&A dry run', zone: 'Comms tent', crew: 'All', desc: 'Three minute pitch, then open questions. Focus on why gas and thermal are read together rather than separately.' },
    ],
  },
  {
    offset: 1,
    slots: [
      { start: T(7, 0),  dur: 60,  kind: 'ops',    title: 'Pre-flight + warm-up', zone: 'Bay 2', crew: 'Mech + Sensors', desc: 'Standard morning block. Add a wheel-encoder check after yesterday’s slip on the berm.' },
      { start: T(8, 30), dur: 180, kind: 'run',    title: 'Full-site sweep', zone: 'Cells A–D', crew: 'Nav + Sensors', desc: 'Complete perimeter and interior transect in one battery cycle. Target: 900+ readings for the heat map.' },
      { start: T(12, 30), dur: 90, kind: 'review', title: 'Hazard map build', zone: 'Comms tent', crew: 'Software', desc: 'Render the accumulated readings into the site hazard overlay for the final report.' },
      { start: T(15, 0), dur: 60,  kind: 'ops',    title: 'Spare parts audit', zone: 'Bay 2', crew: 'Mech', desc: 'Confirm spare MQ modules, one spare MLX90640 breakout, and enough JST leads for a full rebuild.' },
    ],
  },
  {
    offset: 2,
    slots: [
      { start: T(8, 0),  dur: 120, kind: 'run',    title: 'Verification run', zone: 'Cell C4', crew: 'Nav + Sensors', desc: 'Re-drive the two strongest hazard flags from the week to confirm they repeat rather than being sensor drift.' },
      { start: T(11, 0), dur: 120, kind: 'review', title: 'Report writing block', zone: 'Comms tent', crew: 'All', desc: 'Method, results, and the honest limits of a colour-threshold marl estimate.' },
      { start: T(16, 0), dur: 60,  kind: 'deadline', title: 'Submit engineering portfolio', zone: 'Remote', crew: 'All', desc: 'Hard deadline. Portfolio PDF plus the repository link go up together.' },
    ],
  },
  {
    offset: 3,
    slots: [
      { start: T(9, 0),  dur: 90,  kind: 'ops',    title: 'Pack-down + transport prep', zone: 'Bay 2', crew: 'Mech', desc: 'Rover into the flight case. Batteries below 30% for transport, sensors bagged with desiccant.' },
      { start: T(13, 0), dur: 60,  kind: 'review', title: 'Presentation rehearsal', zone: 'Comms tent', crew: 'All', desc: 'Full run-through with the live console on the projector.' },
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
