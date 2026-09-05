/* ---------------------------------------------------------------------------
   Subsystem roster for the STATUS view.

   `read` derives the row's live values from the newest telemetry reading, so
   the status page moves with the same data stream the live view charts.
   --------------------------------------------------------------------------- */

export const SUBSYSTEMS = [
  {
    id: 'mlx',
    name: 'MLX90640 thermal camera',
    bus: 'I²C 0x33 · 8 Hz · 32×24',
    read: (r) => ({
      state: 'online',
      detail: `max ${r.thermal.max_c}°C · avg ${r.thermal.avg_c}°C`,
      num: `${r.thermal.hotspot_count} hs`,
      tone: r.thermal.hotspot_count > 0 ? 'warn' : null,
    }),
  },
  {
    id: 'mq2',
    name: 'MQ-2 smoke',
    bus: 'analog A0 · 16-sample mean',
    read: (r) => ({
      state: 'online',
      detail: 'Rs/R0 curve fit · v2 table',
      num: `${r.gas_ppm.mq2_smoke} ppm`,
      tone: r.gas_ppm.mq2_smoke > 150 ? 'bad' : r.gas_ppm.mq2_smoke > 100 ? 'warn' : null,
    }),
  },
  {
    id: 'mq4',
    name: 'MQ-4 methane',
    bus: 'analog A1 · 16-sample mean',
    read: (r) => ({
      state: 'online',
      detail: 'Rs/R0 curve fit · v2 table',
      num: `${r.gas_ppm.mq4_methane} ppm`,
      tone: r.gas_ppm.mq4_methane > 40 ? 'warn' : null,
    }),
  },
  {
    id: 'mq7',
    name: 'MQ-7 carbon monoxide',
    bus: 'analog A2 · 16-sample mean',
    read: (r) => ({
      state: 'online',
      detail: 'Rs/R0 curve fit · v2 table',
      num: `${r.gas_ppm.mq7_co} ppm`,
      tone: r.gas_ppm.mq7_co > 50 ? 'bad' : r.gas_ppm.mq7_co > 25 ? 'warn' : null,
    }),
  },
  {
    id: 'mq135',
    name: 'MQ-135 air quality',
    bus: 'analog A3 · 16-sample mean',
    read: (r) => ({
      state: 'degraded',
      detail: 'drifting — recalibrate at next bench block',
      num: `${r.gas_ppm.mq135_voc} ppm`,
      tone: 'warn',
    }),
  },
  {
    id: 'cam',
    name: 'OV2640 still camera',
    bus: 'SPI · JPEG 800×600 · 15 s interval',
    read: () => ({ state: 'online', detail: 'last capture uploaded to Storage', num: '38 kB' }),
  },
  {
    id: 'gps',
    name: 'SIM7600G-H GNSS',
    bus: '/dev/ttyUSB1 · 115200 · GPGGA',
    read: (r) => ({
      state: 'online',
      detail: `${r.lat.toFixed(5)}, ${r.lon.toFixed(5)}`,
      num: '9 sats',
    }),
  },
  {
    id: 'link',
    name: 'Cellular uplink → Firestore',
    bus: 'LTE Cat-4 · readings collection',
    read: () => ({ state: 'online', detail: 'writes acknowledged', num: '184 ms' }),
  },
  {
    id: 'arduino',
    name: 'Arduino ↔ Pi serial bridge',
    bus: '/dev/ttyACM0 · 115200 · JSON lines',
    read: (r) => ({ state: 'online', detail: `seq ${r.seq} · no frame errors`, num: '1.0 Hz' }),
  },
  {
    id: 'power',
    name: 'Drive + logic power',
    read: (_r, ctx) => ({
      state: ctx.battery < 20 ? 'degraded' : 'online',
      detail: `${ctx.battery < 20 ? 'return to bay' : 'discharging'} · 11.${Math.round(ctx.battery / 12)} V`,
      num: `${ctx.battery}%`,
      tone: ctx.battery < 20 ? 'bad' : ctx.battery < 40 ? 'warn' : null,
    }),
  },
];

export const STATE_META = {
  online:   { label: 'Online',   tone: 'safe' },
  warmup:   { label: 'Warm-up',  tone: 'amber' },
  degraded: { label: 'Degraded', tone: 'amber' },
  offline:  { label: 'Offline',  tone: 'idle' },
};
