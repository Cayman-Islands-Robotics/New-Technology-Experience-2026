import { Panel } from '../components/Panel.jsx';
import { Tag } from '../components/Tag.jsx';
import { DefList } from '../components/DefList.jsx';
import { DataTable } from '../components/DataTable.jsx';
import { STATE_META, SUBSYSTEMS } from '../data/subsystems.js';
import { HOTSPOT_THRESHOLD_C } from '../data/telemetry.js';
import { READINGS_COLLECTION, firebaseConfig } from '../firebase.js';
import { ago, clockTime, latLon, val } from '../lib/format.js';

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

const CONNECTION = {
  connecting: 'connecting',
  live: 'listening',
  error: 'error — see event log',
  unconfigured: 'no config',
};

export function StatusView({ current, history, level, now, connection }) {
  const summary = SUMMARY[level] ?? SUMMARY.safe;
  const rows = SUBSYSTEMS.map((s) => ({ ...s, ...s.read(current) }));
  const reporting = rows.filter((r) => r.state === 'online').length;

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
            <dt>Last reading</dt>
            <dd>{ago(current.server_time, now)}</dd>
          </div>
          <div className="summary__metric">
            <dt>Last seq</dt>
            <dd>{val(current.seq)}</dd>
          </div>
          <div className="summary__metric">
            <dt>Reporting</dt>
            <dd>
              {reporting}/{rows.length}
            </dd>
          </div>
        </dl>
      </section>

      <div className="grid grid--3">
        <Panel title="Latest document" meta={current.id}>
          <DefList
            rows={[
              ['Server time', current.server_time == null ? '—' : `${new Date(current.server_time).toLocaleDateString()} ${clockTime(current.server_time)}`],
              ['Position', latLon(current, 5)],
              ['Image', current.image_url ? 'attached' : 'none'],
            ]}
          />
        </Panel>

        <Panel title="Source" meta="Firestore">
          <DefList
            rows={[
              ['Project', firebaseConfig.projectId ?? '—'],
              ['Collection', READINGS_COLLECTION],
              ['Readings loaded', String(history.length)],
              ['Listener', CONNECTION[connection] ?? connection],
            ]}
          />
        </Panel>

        <Panel title="Latest capture" meta="image_url">
          {current.image_url ? (
            <img className="capture" src={current.image_url} alt={`Camera still attached to reading ${val(current.seq)}`} />
          ) : (
            <p className="note">No image attached to the latest reading.</p>
          )}
        </Panel>
      </div>

      <Panel title="Subsystems" meta={`hotspot threshold ${HOTSPOT_THRESHOLD_C}°C`} flush>
        <DataTable
          caption="Subsystem status derived from the latest reading"
          columns={COLUMNS}
          rows={rows}
          rowKey={(r) => r.id}
          rowClass={(r) => (r.tone === 'bad' ? 'row--marked' : undefined)}
        />
      </Panel>
    </div>
  );
}
