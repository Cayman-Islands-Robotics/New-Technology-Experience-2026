import { Tag } from './Tag.jsx';
import { clockTime, isoAt } from '../lib/format.js';

const STATE = {
  safe: { level: 'ok', code: 'NOMINAL', text: 'All channels within threshold.' },
  amber: {
    level: 'caution',
    code: 'CAUTION',
    text: 'One channel above threshold without corroboration.',
  },
  ember: {
    level: 'alarm',
    code: 'HAZARD',
    text: 'Gas and thermal above threshold at the same position.',
  },
  idle: { level: 'muted', code: 'STANDBY', text: 'No telemetry.' },
};

export function AppBar({ level, now, warmingUp }) {
  const s = warmingUp
    ? { level: 'caution', code: 'WARMUP', text: 'Gas sensors below operating temperature.' }
    : (STATE[level] ?? STATE.idle);

  return (
    <header className="appbar">
      <div className="appbar__ident">SCOUT-01</div>

      {/* Telemetry-driven; announced politely so it does not interrupt. */}
      <div className="appbar__state" role="status" aria-live="polite" aria-atomic="true">
        <Tag level={s.level}>{s.code}</Tag>
        <span className="appbar__state-text">{s.text}</span>
      </div>

      <div className="appbar__clock">
        <span className="label">Local</span>
        <time dateTime={isoAt(now)}>{clockTime(now)}</time>
      </div>
    </header>
  );
}
