import { useEffect, useState } from 'react';
import { apiGet } from '../api';

interface Component {
  name: string;
  ok: boolean;
  data?: Record<string, unknown>;
  error?: string;
}

interface Overview {
  schema?: string;
  generated_at?: string;
  degraded?: boolean;
  components?: Component[];
}

// S1 dashboard: renders the /v1/console/overview aggregate. The kb fans the
// telemetry read models in-process into a versioned, timestamped envelope with a
// per-component {ok, data|error}; this page renders each component as a card and
// surfaces the overall degraded state.
export default function ConsoleDashboard() {
  const [ov, setOv] = useState<Overview | null>(null);
  const [err, setErr] = useState('');

  async function refresh() {
    try {
      setOv(await apiGet<Overview>('/v1/console/overview'));
      setErr('');
    } catch (e) {
      setErr(String(e));
    }
  }

  useEffect(() => {
    refresh();
    const t = setInterval(refresh, 15000);
    return () => clearInterval(t);
  }, []);

  return (
    <section>
      <header className="kbc-dash-head">
        <h2>Dashboard</h2>
        {ov?.degraded && <span className="kbc-badge kbc-badge-warn">degraded</span>}
        {ov?.generated_at && <span className="kbc-ts">as of {ov.generated_at}</span>}
        <button onClick={refresh}>Refresh</button>
      </header>
      {err && <p className="kbc-error">overview unavailable: {err}</p>}
      <div className="kbc-cards">
        {(ov?.components ?? []).map((c) => (
          <ComponentCard key={c.name} c={c} />
        ))}
      </div>
    </section>
  );
}

function ComponentCard({ c }: { c: Component }) {
  return (
    <div className={`kbc-card ${c.ok ? '' : 'kbc-card-err'}`}>
      <div className="kbc-card-head">
        <span className="kbc-card-title">{c.name}</span>
        <span className={`kbc-badge ${c.ok ? 'kbc-badge-ok' : 'kbc-badge-err'}`}>
          {c.ok ? 'ok' : 'error'}
        </span>
      </div>
      {c.ok ? <DataGrid data={c.data ?? {}} /> : <p className="kbc-error">{c.error}</p>}
    </div>
  );
}

// DataGrid renders a flat object as label/value tiles; nested values are shown
// compactly as JSON so any component shape stays legible without a bespoke panel.
function DataGrid({ data }: { data: Record<string, unknown> }) {
  const entries = Object.entries(data);
  if (entries.length === 0) return <p className="kbc-muted">no data</p>;
  return (
    <dl className="kbc-kv">
      {entries.map(([k, v]) => (
        <div key={k} className="kbc-kv-row">
          <dt>{k}</dt>
          <dd>{typeof v === 'object' && v !== null ? JSON.stringify(v) : String(v)}</dd>
        </div>
      ))}
    </dl>
  );
}
