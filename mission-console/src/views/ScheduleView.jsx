import { useMemo, useState } from 'react';
import { Panel } from '../components/Panel.jsx';
import { Tag } from '../components/Tag.jsx';
import { DataTable } from '../components/DataTable.jsx';
import { KIND_META, SCHEDULE, dayFor, slotStatus } from '../data/schedule.js';
import { durMins, hhmm } from '../lib/format.js';

/* Fixed three-letter forms rather than toLocaleDateString: ICU returns "Sep"
   in Chrome and "Sept" in Node, and a console column must not change width
   with the host's locale data. */
const DOW = ['SUN', 'MON', 'TUE', 'WED', 'THU', 'FRI', 'SAT'];
const MON = ['JAN', 'FEB', 'MAR', 'APR', 'MAY', 'JUN', 'JUL', 'AUG', 'SEP', 'OCT', 'NOV', 'DEC'];

/** Gantt strip window: the working day, in minutes since midnight. */
const DAY_START = 6 * 60;
const DAY_END = 19 * 60;
const DAY_SPAN = DAY_END - DAY_START;

const STATUS_TAG = {
  done: { level: 'muted', label: 'COMPLETE' },
  active: { level: 'alarm', label: 'ACTIVE' },
  upcoming: { level: 'ok', label: 'SCHEDULED' },
};

const COLUMNS = [
  {
    key: 'start',
    label: 'Window',
    width: '108px',
    render: (s) => (
      <span className="mono">
        {hhmm(s.start)}–{hhmm(s.start + s.dur)}
      </span>
    ),
  },
  { key: 'dur', label: 'Dur', numeric: true, width: '56px', render: (s) => durMins(s.dur) },
  {
    key: 'title',
    label: 'Block',
    render: (s) => (
      <>
        <strong>{s.title}</strong>
        <span className="subsys__bus">{s.desc}</span>
      </>
    ),
  },
  {
    key: 'kind',
    label: 'Type',
    width: '92px',
    render: (s) => <Tag level="muted">{KIND_META[s.kind].label.toUpperCase()}</Tag>,
  },
  { key: 'zone', label: 'Zone', width: '104px', render: (s) => <span className="mono">{s.zone}</span> },
  { key: 'crew', label: 'Crew', width: '112px' },
  {
    key: 'status',
    label: 'State',
    width: '104px',
    render: (s) => {
      const t = STATUS_TAG[s.status];
      return <Tag level={t.level}>{t.label}</Tag>;
    },
  },
];

const toPct = (m) => ((m - DAY_START) / DAY_SPAN) * 100;

const TICK_STEP = 120;
/** Minimum clearance between the last generated tick and the closing one. */
const TICK_END_GAP = 90;

/**
 * Hour ticks every two hours, always closed by the window end. A tick landing
 * within TICK_END_GAP of the end is dropped so its label cannot collide with
 * the closing label at narrow viewports.
 */
const TICKS = (() => {
  const out = [];
  for (let m = DAY_START; m < DAY_END - TICK_END_GAP; m += TICK_STEP) out.push(m);
  out.push(DAY_END);
  return out;
})();

/** Keep the first and last labels inside the track instead of centring them. */
function tickShift(p) {
  if (p <= 0.01) return 'translateX(0)';
  if (p >= 99.99) return 'translateX(-100%)';
  return 'translateX(-50%)';
}

/** Proportional occupancy bar for the selected day. */
function GanttStrip({ slots, showNow, nowMins }) {
  const nowVisible = showNow && nowMins >= DAY_START && nowMins <= DAY_END;
  const nowPct = toPct(nowMins);

  return (
    <>
      <div
        className="gantt"
        role="img"
        aria-label={`${slots.length} blocks scheduled between ${hhmm(DAY_START)} and ${hhmm(DAY_END)}.${
          nowVisible ? ` Current time ${hhmm(nowMins)}.` : ''
        }`}
      >
        {TICKS.slice(1, -1).map((m) => (
          <div key={m} className="gantt__gridline" style={{ left: `${toPct(m)}%` }} />
        ))}

        {slots.map((s) => {
          // Clamp to the window so a block running past 19:00 cannot overflow.
          const left = Math.max(0, toPct(s.start));
          const right = Math.min(100, toPct(s.start + s.dur));
          return (
            <div
              key={`${s.start}-${s.title}`}
              className={`gantt__block${
                s.status === 'done'
                  ? ' gantt__block--done'
                  : s.status === 'active'
                    ? ' gantt__block--marked'
                    : ''
              }`}
              style={{ left: `${left}%`, width: `${Math.max(0, right - left)}%` }}
              title={`${hhmm(s.start)}\u2013${hhmm(s.start + s.dur)}  ${s.title}`}
            />
          );
        })}

        {nowVisible && <div className="gantt__now" style={{ left: `${nowPct}%` }} />}
      </div>

      <div className="gantt__axis" aria-hidden="true">
        {TICKS.map((m) => {
          const p = toPct(m);
          return (
            <span key={m} className="gantt__tick" style={{ left: `${p}%`, transform: tickShift(p) }}>
              {hhmm(m)}
            </span>
          );
        })}
      </div>
    </>
  );
}

export function ScheduleView({ now }) {
  const [offset, setOffset] = useState(0);
  const d = new Date(now);
  const nowMins = d.getHours() * 60 + d.getMinutes();

  const day = SCHEDULE.find((x) => x.offset === offset) ?? SCHEDULE[0];

  const slots = useMemo(
    () => day.slots.map((s) => ({ ...s, status: slotStatus(s, day.offset, nowMins) })),
    [day, nowMins]
  );

  const sortieMins = slots.filter((s) => s.kind === 'run').reduce((a, s) => a + s.dur, 0);
  const done = slots.filter((s) => s.status === 'done').length;

  return (
    <div className="stack">
      <div className="segmented" role="group" aria-label="Select schedule day">
        {SCHEDULE.map((x) => {
          const date = dayFor(x.offset);
          const runs = x.slots.filter((s) => s.kind === 'run').length;
          return (
            <button
              key={x.offset}
              type="button"
              className="segmented__item"
              aria-pressed={x.offset === offset}
              onClick={() => setOffset(x.offset)}
            >
              <span className="segmented__label">{x.offset === 0 ? 'TODAY' : DOW[date.getDay()]}</span>
              <span className="segmented__value">
                {String(date.getDate()).padStart(2, '0')} {MON[date.getMonth()]}
              </span>
              <span className="segmented__meta">
                {x.slots.length} blocks · {runs} sortie{runs === 1 ? '' : 's'}
              </span>
            </button>
          );
        })}
      </div>

      <Panel
        title="Day occupancy"
        meta={
          day.offset === 0
            ? `${hhmm(DAY_START)} – ${hhmm(DAY_END)} · now ${hhmm(nowMins)}`
            : `${hhmm(DAY_START)} – ${hhmm(DAY_END)}`
        }
      >
        <GanttStrip slots={slots} showNow={day.offset === 0} nowMins={nowMins} />
      </Panel>

      <Panel
        title={
          offset === 0
            ? 'Today'
            : dayFor(offset).toLocaleDateString([], { weekday: 'long', month: 'long', day: 'numeric' })
        }
        meta={`${slots.length} blocks · ${durMins(sortieMins)} sortie time · ${done} complete`}
        flush
      >
        <DataTable
          caption="Scheduled mission blocks for the selected day"
          columns={COLUMNS}
          rows={slots}
          rowKey={(s) => `${s.start}-${s.title}`}
          rowClass={(s) =>
            s.status === 'active' ? 'row--marked' : s.status === 'done' ? 'row--done' : undefined
          }
        />
      </Panel>
    </div>
  );
}
