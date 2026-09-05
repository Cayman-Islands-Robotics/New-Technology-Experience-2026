import { Panel } from '../components/Panel.jsx';
import { Readout } from '../components/Readout.jsx';
import { Meter } from '../components/Meter.jsx';
import { DataTable } from '../components/DataTable.jsx';
import { TrendChart } from '../components/TrendChart.jsx';
import { ThermalMap } from '../components/ThermalMap.jsx';
import { HOTSPOT_THRESHOLD_C, hazardLevel } from '../data/telemetry.js';
import { clockTime, isoAt } from '../lib/format.js';

/* Alarm thresholds, mirrored from the dashboard's hazard rules. */
const LIMIT = { smoke: 150, co: 50, methane: 40, voc: 450 };

const SERIES = [
  { label: 'Smoke (MQ-2)', unit: ' ppm', get: (r) => r.gas_ppm.mq2_smoke },
  { label: 'Thermal max', unit: ' °C', get: (r) => r.thermal.max_c },
];

const FEED_COLUMNS = [
  {
    key: 'time',
    label: 'Time',
    render: (r) => <time dateTime={isoAt(r.server_time)}>{clockTime(r.server_time)}</time>,
  },
  { key: 'seq', label: 'Seq', numeric: true },
  {
    key: 'smoke',
    label: 'Smoke',
    numeric: true,
    render: (r) => r.gas_ppm.mq2_smoke,
    cellClass: (r) => (r.gas_ppm.mq2_smoke > LIMIT.smoke ? 'cell--breach' : undefined),
  },
  { key: 'ch4', label: 'CH₄', numeric: true, render: (r) => r.gas_ppm.mq4_methane },
  {
    key: 'co',
    label: 'CO',
    numeric: true,
    render: (r) => r.gas_ppm.mq7_co,
    cellClass: (r) => (r.gas_ppm.mq7_co > LIMIT.co ? 'cell--breach' : undefined),
  },
  { key: 'voc', label: 'VOC', numeric: true, render: (r) => r.gas_ppm.mq135_voc },
  {
    key: 'max',
    label: 'Max °C',
    numeric: true,
    render: (r) => r.thermal.max_c,
    cellClass: (r) => (r.thermal.max_c > HOTSPOT_THRESHOLD_C ? 'cell--breach' : undefined),
  },
  { key: 'hs', label: 'Hotspots', numeric: true, render: (r) => r.thermal.hotspot_count },
  {
    key: 'pos',
    label: 'Position',
    render: (r) => `${r.lat.toFixed(4)}, ${r.lon.toFixed(4)}`,
  },
];

export function LiveView({ current, history, log }) {
  const g = current.gas_ppm;
  const t = current.thermal;
  const level = hazardLevel(current);
  const feed = history.slice(-14).reverse();

  return (
    <div className="grid grid--live">
      <div className="stack">
        <Panel title="Live readings" meta={`seq ${current.seq} · ${clockTime(current.server_time)}`}>
          {/* Values change on a 2 s cadence; announcing every one would be noise,
              so the region is silent and the alarm summary lives in the app bar. */}
          <div className="readout-grid" aria-live="off">
            <Readout
              label="Smoke MQ-2"
              value={g.mq2_smoke}
              unit="ppm"
              note={`limit ${LIMIT.smoke}`}
              breach={g.mq2_smoke > LIMIT.smoke}
            />
            <Readout
              label="Methane MQ-4"
              value={g.mq4_methane}
              unit="ppm"
              note={`limit ${LIMIT.methane}`}
              breach={g.mq4_methane > LIMIT.methane}
            />
            <Readout
              label="CO MQ-7"
              value={g.mq7_co}
              unit="ppm"
              note={`limit ${LIMIT.co}`}
              breach={g.mq7_co > LIMIT.co}
            />
            <Readout
              label="Air quality MQ-135"
              value={g.mq135_voc}
              unit="ppm"
              note="sensor drifting"
              breach={g.mq135_voc > LIMIT.voc}
            />
            <Readout
              label="Thermal max"
              value={t.max_c}
              unit="°C"
              note={`avg ${t.avg_c} · min ${t.min_c}`}
              breach={t.max_c > HOTSPOT_THRESHOLD_C}
            />
            <Readout
              label="Hotspots"
              value={t.hotspot_count}
              note={`px over ${HOTSPOT_THRESHOLD_C}°C`}
              breach={t.hotspot_count > 0}
            />
          </div>
        </Panel>

        <Panel title="Trend" meta="last 60 readings · 2 s cadence">
          <TrendChart
            rows={history.slice(-60)}
            series={SERIES}
            threshold={{ value: LIMIT.smoke, label: `Smoke limit ${LIMIT.smoke} ppm`, seriesIndex: 0 }}
          />
        </Panel>

        <Panel title="Reading feed" meta="newest first" flush>
          <DataTable
            caption="Most recent sensor readings, newest first"
            columns={FEED_COLUMNS}
            rows={feed}
            rowKey={(r) => r.seq}
            rowClass={(r) => (hazardLevel(r) === 'ember' ? 'row--marked' : undefined)}
          />
        </Panel>
      </div>

      <div className="stack">
        <Panel title="Thermal frame" meta="MLX90640 · 32 × 24">
          <ThermalMap reading={current} />
          <p className="note" style={{ marginTop: 'var(--s2)' }}>
            {t.hotspot_count
              ? `${t.hotspot_count} px above ${HOTSPOT_THRESHOLD_C}°C; first at (${t.hotspot_px[0][0]}, ${t.hotspot_px[0][1]}).`
              : `No pixel above the ${HOTSPOT_THRESHOLD_C}°C hotspot threshold in this frame.`}
          </p>
        </Panel>

        <Panel title="Marl coverage" meta="colour-threshold v1">
          <p className="readout__value" style={{ marginBottom: 'var(--s2)' }}>
            {current.marl_pct}
            <span className="readout__unit">%</span>
          </p>
          <Meter value={current.marl_pct} label="Estimated marl coverage" />
          <p className="note" style={{ marginTop: 'var(--s2)' }}>
            Estimated from the latest capture by an HSV brightness and saturation window. A trained
            classifier replaces this without changing the rest of the pipeline.
          </p>
        </Panel>

        <Panel title="Event log" meta={level === 'ember' ? 'hazard active' : 'nominal'} flush>
          <div className="log" role="log" aria-label="Event log" aria-live="polite">
            {log.map((e) => (
              <div
                key={e.id}
                className={`log__row${e.tone === 'alert' ? ' log__row--alarm' : e.tone === 'warn' ? ' log__row--caution' : ''}`}
              >
                <time className="log__time" dateTime={isoAt(e.t)}>
                  {clockTime(e.t)}
                </time>
                <span className="log__text">{e.text}</span>
              </div>
            ))}
          </div>
        </Panel>
      </div>
    </div>
  );
}
