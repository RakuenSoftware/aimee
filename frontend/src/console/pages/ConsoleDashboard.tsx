import { useEffect, useState } from 'react';
import { apiGet } from '../api';

// S0 placeholder. S1 replaces this with the /v1/console/overview panels
// (Pipeline / Knowledge / Health / Version) rendered from the aggregate.
interface Overview {
  schema?: string;
  degraded?: boolean;
  components?: unknown[];
}

export default function ConsoleDashboard() {
  const [ov, setOv] = useState<Overview | null>(null);
  const [err, setErr] = useState('');

  useEffect(() => {
    apiGet<Overview>('/v1/console/overview')
      .then(setOv)
      .catch((e) => setErr(String(e)));
  }, []);

  return (
    <section>
      <h2>Dashboard</h2>
      {err && <p className="kbc-error">overview unavailable: {err}</p>}
      {ov && (
        <p>
          connected — schema <code>{ov.schema}</code>, {ov.components?.length ?? 0} components
          {ov.degraded ? ' (degraded)' : ''}. Panels land in S1.
        </p>
      )}
    </section>
  );
}
