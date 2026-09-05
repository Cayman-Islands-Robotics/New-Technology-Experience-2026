import { useState } from 'react';
import { AppBar } from './components/AppBar.jsx';
import { TabNav, TabPanel } from './components/TabNav.jsx';
import { StatusView } from './views/StatusView.jsx';
import { ScheduleView } from './views/ScheduleView.jsx';
import { LiveView } from './views/LiveView.jsx';
import { useTelemetry } from './useTelemetry.js';
import { SUBSYSTEMS } from './data/subsystems.js';
import { SCHEDULE, slotStatus } from './data/schedule.js';

const TABS = [
  { id: 'status', label: 'Status' },
  { id: 'schedule', label: 'Schedule' },
  { id: 'live', label: 'Live data' },
];

export default function App() {
  const [tab, setTab] = useState('status');
  const t = useTelemetry();

  const d = new Date(t.now);
  const nowMins = d.getHours() * 60 + d.getMinutes();
  const today = SCHEDULE.find((x) => x.offset === 0);
  const remaining = today.slots.filter((s) => slotStatus(s, 0, nowMins) !== 'done').length;

  const tabs = TABS.map((x) => ({
    ...x,
    count: x.id === 'status' ? SUBSYSTEMS.length : x.id === 'schedule' ? remaining : null,
  }));

  return (
    <div className="app">
      <a className="skip-link" href="#main">
        Skip to content
      </a>

      <AppBar level={t.level} now={t.now} warmingUp={t.warmingUp} />
      <TabNav tabs={tabs} active={tab} onChange={setTab} label="Console sections" />

      <main id="main" className="panelregion">
        <TabPanel id="status" active={tab === 'status'}>
          <StatusView {...t} />
        </TabPanel>
        <TabPanel id="schedule" active={tab === 'schedule'}>
          <ScheduleView now={t.now} />
        </TabPanel>
        <TabPanel id="live" active={tab === 'live'}>
          <LiveView current={t.current} history={t.history} log={t.log} />
        </TabPanel>
      </main>
    </div>
  );
}
