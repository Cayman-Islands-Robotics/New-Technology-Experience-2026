import { useEffect, useRef, useState } from 'react';
import {
  TELEMETRY_INTERVAL_MS,
  WARMUP_MS,
  hazardLevel,
  makeReading,
  seedHistory,
} from './data/telemetry.js';

const HISTORY_LEN = 90;
const LOG_LEN = 60;

let logId = 0;

function logFor(reading, prevLevel) {
  const level = hazardLevel(reading);
  const t = reading.server_time;
  if (level === 'ember' && prevLevel !== 'ember') {
    return { id: ++logId, t, tone: 'alert', text: `HAZARD FLAG · smoke ${reading.gas_ppm.mq2_smoke} ppm + ${reading.thermal.max_c}°C` };
  }
  if (level === 'amber' && prevLevel === 'safe') {
    return { id: ++logId, t, tone: 'warn', text: `single-sensor flag · seq ${reading.seq}` };
  }
  if (level === 'safe' && prevLevel !== 'safe') {
    return { id: ++logId, t, tone: null, text: `cleared · back to baseline` };
  }
  if (reading.seq % 5 === 0) {
    return { id: ++logId, t, tone: null, text: `reading logged · seq ${reading.seq}` };
  }
  return null;
}

/**
 * Drives the whole console off one simulated telemetry stream.
 * Replacing this with a Firestore onSnapshot listener needs no other changes.
 */
export function useTelemetry({ live = true } = {}) {
  const seed = useRef(null);
  if (seed.current === null) seed.current = seedHistory(HISTORY_LEN);

  const walker = useRef(seed.current.state);
  const bootedAt = useRef(Date.now());
  const prevLevel = useRef('safe');

  const [history, setHistory] = useState(seed.current.history);
  const [current, setCurrent] = useState(() => makeReading(seed.current.state));
  const [log, setLog] = useState(() => [
    { id: ++logId, t: Date.now(), tone: null, text: 'console attached · replaying mock stream' },
  ]);
  const [now, setNow] = useState(() => Date.now());

  // Telemetry tick.
  useEffect(() => {
    if (!live) return undefined;
    const id = setInterval(() => {
      const reading = makeReading(walker.current);
      setCurrent(reading);
      setHistory((h) => {
        const slim = { ...reading, thermal: { ...reading.thermal, frame: undefined } };
        const next = h.concat(slim);
        return next.length > HISTORY_LEN ? next.slice(next.length - HISTORY_LEN) : next;
      });
      const entry = logFor(reading, prevLevel.current);
      prevLevel.current = hazardLevel(reading);
      if (entry) setLog((l) => [entry, ...l].slice(0, LOG_LEN));
    }, TELEMETRY_INTERVAL_MS);
    return () => clearInterval(id);
  }, [live]);

  // Wall clock, for the header and the schedule's "now" marker.
  useEffect(() => {
    const id = setInterval(() => setNow(Date.now()), 1000);
    return () => clearInterval(id);
  }, []);

  const uptimeMs = now - bootedAt.current;
  const warmupLeftMs = Math.max(0, WARMUP_MS - uptimeMs);

  return {
    current,
    history,
    log,
    now,
    uptimeMs,
    warmupLeftMs,
    warmingUp: warmupLeftMs > 0,
    level: hazardLevel(current),
    // Battery: a plausible slow drain from a full pack at console start.
    battery: Math.max(6, Math.round(94 - uptimeMs / 60000 * 1.4)),
  };
}
