import { useEffect, useRef, useState } from 'react';
import { collection, limit, onSnapshot, orderBy, query } from 'firebase/firestore';
import { READINGS_COLLECTION, db } from './firebase.js';
import { fromSnapshot, hazardLevel } from './data/telemetry.js';

const HISTORY_LEN = 90;
const LOG_LEN = 60;

let logId = 0;

function logFor(reading, prevLevel) {
  const level = hazardLevel(reading);
  const t = reading.server_time ?? Date.now();
  if (level === 'ember' && prevLevel !== 'ember') {
    return { id: ++logId, t, tone: 'alert', text: `HAZARD FLAG · smoke ${reading.gas_ppm.mq2_smoke} ppm + ${reading.thermal.max_c}°C` };
  }
  if (level === 'amber' && prevLevel === 'safe') {
    return { id: ++logId, t, tone: 'warn', text: `single-sensor flag · seq ${reading.seq}` };
  }
  if (level === 'safe' && prevLevel !== 'safe') {
    return { id: ++logId, t, tone: null, text: `cleared · back to baseline` };
  }
  return { id: ++logId, t, tone: null, text: `reading received · seq ${reading.seq}` };
}

/**
 * Live view of the newest documents in the Firestore `readings` collection.
 * `current` is null until the first document arrives.
 */
export function useTelemetry() {
  const prevLevel = useRef('safe');

  const [history, setHistory] = useState([]);
  const [current, setCurrent] = useState(null);
  const [connection, setConnection] = useState(db ? 'connecting' : 'unconfigured');
  const [log, setLog] = useState(() => [
    db
      ? { id: ++logId, t: Date.now(), tone: null, text: `connecting · ${READINGS_COLLECTION} collection` }
      : { id: ++logId, t: Date.now(), tone: 'alert', text: 'no Firebase config · see .env.example' },
  ]);
  const [now, setNow] = useState(() => Date.now());

  useEffect(() => {
    if (!db) return undefined;
    const q = query(
      collection(db, READINGS_COLLECTION),
      orderBy('server_time', 'desc'),
      limit(HISTORY_LEN)
    );
    let first = true;
    return onSnapshot(
      q,
      (snap) => {
        const newestFirst = snap.docs.map(fromSnapshot);
        setHistory(newestFirst.slice().reverse());
        setCurrent(newestFirst[0] ?? null);
        setConnection('live');

        if (first) {
          first = false;
          prevLevel.current = hazardLevel(newestFirst[0]);
          const text = `connected · ${snap.size} reading${snap.size === 1 ? '' : 's'} loaded`;
          setLog((l) => [{ id: ++logId, t: Date.now(), tone: null, text }, ...l].slice(0, LOG_LEN));
          return;
        }

        const added = snap
          .docChanges()
          .filter((c) => c.type === 'added')
          .map((c) => fromSnapshot(c.doc))
          .reverse(); // oldest first, so hazard transitions log in order
        const entries = added.map((r) => {
          const entry = logFor(r, prevLevel.current);
          prevLevel.current = hazardLevel(r);
          return entry;
        });
        if (entries.length) setLog((l) => [...entries.reverse(), ...l].slice(0, LOG_LEN));
      },
      (err) => {
        console.error(err);
        setConnection('error');
        setLog((l) => [{ id: ++logId, t: Date.now(), tone: 'alert', text: `Firestore error · ${err.code ?? err.message}` }, ...l].slice(0, LOG_LEN));
      }
    );
  }, []);

  // Wall clock, for the header and the schedule's "now" marker.
  useEffect(() => {
    const id = setInterval(() => setNow(Date.now()), 1000);
    return () => clearInterval(id);
  }, []);

  return {
    current,
    history,
    log,
    now,
    connection,
    level: hazardLevel(current),
  };
}
