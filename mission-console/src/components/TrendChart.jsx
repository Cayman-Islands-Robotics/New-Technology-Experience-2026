import { useMemo } from 'react';
import { useElementWidth } from '../lib/useElementWidth.js';

/* ---------------------------------------------------------------------------
   Dual-scale line chart in raw SVG. No charting dependency.

   The SVG is laid out in real CSS pixels — measured, not scaled. An earlier
   revision used a fixed viewBox with preserveAspectRatio="none", which stretches
   every glyph horizontally once the container is wider than the viewBox.
   `non-scaling-stroke` saved the lines but not the text.

   Series are distinguished by stroke pattern, not hue; the accent is spent only
   on the alarm threshold rule. ppm and °C cannot share a range, so each series
   carries its own scale and the axis it is labelled against.
   --------------------------------------------------------------------------- */

const H = 172;
const PAD = { top: 12, right: 44, bottom: 22, left: 44 };
const TICKS = [0, 0.25, 0.5, 0.75, 1];

/** Bounds with headroom, floored at zero for physical quantities. */
function boundsOf(values) {
  const finite = values.filter(Number.isFinite);
  if (!finite.length) return { lo: 0, hi: 1 };
  const min = Math.min(...finite);
  const max = Math.max(...finite);
  const pad = (max - min) * 0.15 || Math.max(1, max * 0.1);
  const lo = Math.max(0, min - pad);
  const hi = max + pad;
  return { lo, hi: hi === lo ? lo + 1 : hi };
}

export function TrendChart({ rows, series, threshold }) {
  const [wrapRef, width] = useElementWidth();

  const innerW = Math.max(40, width - PAD.left - PAD.right);
  const innerH = H - PAD.top - PAD.bottom;
  const projectY = (v, b) => PAD.top + innerH - ((v - b.lo) / (b.hi - b.lo)) * innerH;

  const model = useMemo(() => {
    return series.map((s) => {
      const values = rows.map(s.get);
      const bounds = boundsOf(values);
      const d =
        values.length < 2
          ? ''
          : values
              .map((v, i) => {
                const x = PAD.left + (i / (values.length - 1)) * innerW;
                const y = PAD.top + innerH - ((v - bounds.lo) / (bounds.hi - bounds.lo)) * innerH;
                return `${i === 0 ? 'M' : 'L'}${x.toFixed(1)},${y.toFixed(1)}`;
              })
              .join(' ');
      return { ...s, values, bounds, d, last: values.at(-1) };
    });
  }, [rows, series, innerW, innerH]);

  const [primary, secondary] = model;
  if (!primary) return null;

  const right = PAD.left + innerW;
  const bottom = PAD.top + innerH;

  const describe = (s) =>
    `${s.label} from ${Math.min(...s.values)} to ${Math.max(...s.values)} ${s.unit.trim()}, latest ${s.last}`;

  return (
    <figure className="chart" ref={wrapRef}>
      <div className="chart__legend">
        {model.map((s, i) => (
          <span key={s.label}>
            <i className={`chart__swatch${i > 0 ? ' chart__swatch--dashed' : ''}`} />
            {s.label} <strong>{s.last}{s.unit}</strong>
          </span>
        ))}
        {threshold && (
          <span>
            <i className="chart__swatch chart__swatch--accent chart__swatch--dashed" />
            {threshold.label}
          </span>
        )}
      </div>

      {/* 1:1 — viewBox matches the pixel box, so nothing is scaled. */}
      <svg
        width={width}
        height={H}
        viewBox={`0 0 ${width} ${H}`}
        role="img"
        aria-label={model.map(describe).join('. ')}
      >
        {TICKS.map((f) => {
          const y = PAD.top + innerH * f;
          const pv = primary.bounds.hi - (primary.bounds.hi - primary.bounds.lo) * f;
          return (
            <g key={f}>
              {/* gridline, skipped on the baseline where the axis already sits */}
              {f < 1 && <line x1={PAD.left} y1={y} x2={right} y2={y} className="chart__grid" />}
              <text x={PAD.left - 7} y={y + 3.5} textAnchor="end" className="chart__tick">
                {Math.round(pv)}
              </text>
              {secondary && (
                <>
                  <line x1={right} y1={y} x2={right + 4} y2={y} className="chart__axis" />
                  <text x={right + 8} y={y + 3.5} className="chart__tick">
                    {Math.round(
                      secondary.bounds.hi - (secondary.bounds.hi - secondary.bounds.lo) * f
                    )}
                  </text>
                </>
              )}
            </g>
          );
        })}

        {threshold &&
          (() => {
            const target = model[threshold.seriesIndex ?? 0];
            const y = projectY(threshold.value, target.bounds);
            if (y < PAD.top || y > bottom) return null;
            return <line x1={PAD.left} y1={y} x2={right} y2={y} className="chart__threshold" />;
          })()}

        {model.map((s, i) => (
          <path key={s.label} d={s.d} className={`chart__line${i > 0 ? ' chart__line--dashed' : ''}`} />
        ))}

        {/* Open frame: value axis and baseline only, no enclosing box. */}
        <line x1={PAD.left} y1={PAD.top} x2={PAD.left} y2={bottom} className="chart__axis" />
        <line x1={PAD.left} y1={bottom} x2={right} y2={bottom} className="chart__axis" />
      </svg>

      <figcaption className="visually-hidden">
        Trend of {model.map((s) => s.label).join(' and ')} over the most recent readings.
      </figcaption>
    </figure>
  );
}
