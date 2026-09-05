import { Panel } from '../components/Panel.jsx';
import { Tag } from '../components/Tag.jsx';
import { Meter } from '../components/Meter.jsx';
import { DefList } from '../components/DefList.jsx';
import { DataTable } from '../components/DataTable.jsx';
import { STATE_META, SUBSYSTEMS } from '../data/subsystems.js';
import { HOTSPOT_THRESHOLD_C, WARMUP_MS } from '../data/telemetry.js';
import { duration } from '../lib/format.js';

const SUMMARY = {
  safe: {
    alarm: false,
    title: 'Nominal',
    body: 'Gas array and thermal camera agree. Nothing on this transect is above the hazard thresholds.',
  },
  amber: {
    alarm: false,
    title: 'Caution — single-sensor flag',
    body: 'One channel is above threshold without corroboration from the other. Typically drift or a transient; continue the transect and check whether it repeats.',
  },
  ember: {
    alarm: true,
    title: 'Hazard — correlated detection',
    body: 'Smoke or CO is elevated at the same time and position as a thermal hotspot. That correlation is the signature of a subsurface burn rather than sensor noise.',
  },
};

/** Maps a subsystem row's operating state onto a display tag. */
const TAG_LEVEL = { safe: 'ok', amber: 'caution', idle: 'muted' };

const COLUMNS = [
  {
    key: 'name',
    label: 'Subsystem',
    render: (r) => (
      <>
        {r.name}
        <small className="subsys__bus">{r.bus}</small>
      </>
    ),
  },
  { key: 'detail', label: 'Detail', render: (r) => <span className="mono">{r.detail}</span> },
  {
    key: 'num',
    label: 'Value',
    numeric: true,
    cellClass: (r) => (r.tone === 'bad' ? 'cell--breach' : undefined),
  },
  {
    key: 'state',
    label: 'State',
    render: (r) => {
      const meta = STATE_META[r.state] ?? STATE_META.offline;
      return <Tag level={TAG_LEVEL[meta.tone] ?? 'muted'}>{meta.label.toUpperCase()}</Tag>;
    },
  },
];

export function StatusView({ current, level, uptimeMs, warmupLeftMs, warmingUp, battery }) {
  const summary = SUMMARY[level] ?? SUMMARY.safe;
  const rows = SUBSYSTEMS.map((s) => {
    const r = s.read(current, { battery });
    // Gas channels report warm-up until the firmware's 180 s soak completes.
    const state = warmingUp && s.id.startsWith('mq') ? 'warmup' : r.state;
    return { ...s, ...r, state };
  });

  const impaired = rows.filter((r) => r.state === 'degraded' || r.state === 'offline').length;
  const warmupPct = ((WARMUP_MS - warmupLeftMs) / WARMUP_MS) * 100;
  const batteryLow = battery < 20;

  return (
    <div className="stack">
      <section
        className={summary.alarm ? 'summary summary--alarm' : 'summary'}
        aria-label="Overall vehicle state"
      >
        <div className="summary__text">
          <h1 className={summary.alarm ? 'summary__title summary__title--alarm' : 'summary__title'}>
            {summary.title}
          </h1>
          <p className="summary__body">{summary.body}</p>
        </div>
        <dl className="summary__metrics">
          <div className="summary__metric">
            <dt>Uptime</dt>
            <dd>{duration(uptimeMs)}</dd>
          </div>
          <div className="summary__metric">
            <dt>Last seq</dt>
            <dd>{current.seq}</dd>
          </div>
          <div className="summary__metric">
            <dt>Subsystems</dt>
            <dd>
              {rows.length - impaired}/{rows.length}
            </dd>
          </div>
        </dl>
      </section>

      <div className="grid grid--3">
        <Panel title="Gas sensor warm-up" meta="firmware · 180 s soak">
          <p className="readout__value" style={{ marginBottom: 'var(--s2)' }}>
            {warmingUp ? `${Math.ceil(warmupLeftMs / 1000)}s` : 'READY'}
          </p>
          <Meter value={warmupPct} label="Gas sensor warm-up progress" />
          <p className="note" style={{ marginTop: 'var(--s2)' }}>
            MQ heaters need three minutes at temperature before a reading is meaningful. The sketch
            withholds telemetry until the soak completes.
          </p>
        </Panel>

        <Panel title="Uplink" meta="LTE Cat-4">
          <p className="readout__value" style={{ marginBottom: 'var(--s2)' }}>
            184<span className="readout__unit">ms</span>
          </p>
          <DefList
            rows={[
              ['Round trip', 'write ack'],
              ['Project', 'thermal-rover'],
              ['Collection', 'readings'],
              ['Queued writes', '0'],
            ]}
          />
        </Panel>

        <Panel title="Power" meta="2 × 5000 mAh · 11.1 V">
          <p
            className={batteryLow ? 'readout__value readout__value--breach' : 'readout__value'}
            style={{ marginBottom: 'var(--s2)' }}
          >
            {battery}
            <span className="readout__unit">%</span>
          </p>
          <Meter value={battery} label="Pack charge remaining" alarm={batteryLow} />
          <p className="note" style={{ marginTop: 'var(--s2)' }}>
            Approximately {Math.max(0, Math.round(battery * 1.6))} min of drive time at the current
            draw. Return to Bay 2 below 20%.
          </p>
        </Panel>
      </div>

      <Panel title="Subsystems" meta={`hotspot threshold ${HOTSPOT_THRESHOLD_C}°C`} flush>
        <DataTable
          caption="Subsystem status, live values and operating state"
          columns={COLUMNS}
          rows={rows}
          rowKey={(r) => r.id}
          rowClass={(r) => (r.tone === 'bad' ? 'row--marked' : undefined)}
        />
      </Panel>
    </div>
  );
}
