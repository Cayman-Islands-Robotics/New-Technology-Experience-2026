/* ---------------------------------------------------------------------------
   Subsystem roster for the STATUS view.

   `read` derives the row from the newest document in `readings`. A subsystem
   is "reporting" when its fields are present in that document and "no data"
   when they are not — nothing outside the collection is shown.
   --------------------------------------------------------------------------- */

import { latLon, val } from '../lib/format.js';

const has = (v) => v != null;

const gasRow = (key, tone) => (r) => {
  const v = r.gas_ppm[key];
  return {
    state: has(v) ? 'online' : 'offline',
    detail: has(v) ? 'gas_ppm.' + key : 'not in document',
    num: val(v, ' ppm'),
    tone: has(v) ? tone(v) : null,
  };
};

export const SUBSYSTEMS = [
  {
    id: 'mlx',
    name: 'MLX90640 thermal camera',
    bus: 'I²C 0x33 · 32×24',
    read: (r) => ({
      state: has(r.thermal.max_c) ? 'online' : 'offline',
      detail: `max ${val(r.thermal.max_c, '°C')} · avg ${val(r.thermal.avg_c, '°C')}`,
      num: `${val(r.thermal.hotspot_count)} hs`,
      tone: r.thermal.hotspot_count > 0 ? 'warn' : null,
    }),
  },
  {
    id: 'mq2',
    name: 'MQ-2 smoke',
    bus: 'analog A0',
    read: gasRow('mq2_smoke', (v) => (v > 150 ? 'bad' : v > 100 ? 'warn' : null)),
  },
  {
    id: 'mq4',
    name: 'MQ-4 methane',
    bus: 'analog A1',
    read: gasRow('mq4_methane', (v) => (v > 40 ? 'warn' : null)),
  },
  {
    id: 'mq7',
    name: 'MQ-7 carbon monoxide',
    bus: 'analog A2',
    read: gasRow('mq7_co', (v) => (v > 50 ? 'bad' : v > 25 ? 'warn' : null)),
  },
  {
    id: 'mq135',
    name: 'MQ-135 air quality',
    bus: 'analog A3',
    read: gasRow('mq135_voc', () => null),
  },
  {
    id: 'cam',
    name: 'OV2640 still camera',
    bus: 'SPI · JPEG → Storage',
    read: (r) => ({
      state: r.image_url ? 'online' : 'offline',
      detail: r.image_url ? 'image_url present' : 'no image_url',
      num: '—',
    }),
  },
  {
    id: 'gps',
    name: 'SIM7600G-H GNSS',
    bus: '/dev/ttyUSB1 · GPGGA',
    read: (r) => ({
      state: has(r.lat) && has(r.lon) ? 'online' : 'offline',
      detail: latLon(r, 5),
      num: '—',
    }),
  },
  {
    id: 'arduino',
    name: 'Arduino ↔ Pi serial bridge',
    bus: '/dev/ttyACM0 · JSON lines',
    read: (r) => ({
      state: has(r.seq) ? 'online' : 'offline',
      detail: `seq ${val(r.seq)}`,
      num: '—',
    }),
  },
];

export const STATE_META = {
  online:  { label: 'Reporting', tone: 'safe' },
  offline: { label: 'No data',   tone: 'idle' },
};
