import { useCallback, useEffect, useState } from 'react';
import { apiGet, apiSend } from '../api';

// S3 Accounts UI over the S2a backend: the issued-cert enrollment table with
// per-row revoke, and the scope lattice. (Enroll-a-client / mint lands once the
// backend adds console-admin to the /v1/enroll allowlist; the OIDC config editor
// lands with S2b.)

interface Enrollment {
  id: number;
  scope: string;
  fingerprint: string;
  serial: string;
  state: 'active' | 'revoked' | string;
  issued_at: string;
  last_seen_at: string;
  expires_at: string;
  revoked_at: string;
  legacy: boolean;
}

interface ScopeRow {
  scope: string;
  count: number;
  active: number;
}

interface OidcConfig {
  issuer: string;
  audience: string;
  jwks_url: string;
  admin_claim: string;
  admin_values: string[];
  updated_at: string;
  configured: boolean;
}

// S2b OIDC editor: the console reads this config from the kb at startup, so a
// change takes effect after a console restart.
function OidcEditor() {
  const [cfg, setCfg] = useState<OidcConfig | null>(null);
  const [form, setForm] = useState({ issuer: '', audience: '', jwks_url: '', admin_claim: '', admin_values: '' });
  const [msg, setMsg] = useState('');
  const [err, setErr] = useState('');

  const load = useCallback(async () => {
    try {
      const c = await apiGet<OidcConfig>('/v1/config/oidc');
      setCfg(c);
      setForm({
        issuer: c.issuer,
        audience: c.audience,
        jwks_url: c.jwks_url,
        admin_claim: c.admin_claim,
        admin_values: (c.admin_values ?? []).join(', '),
      });
      setErr('');
    } catch (e) {
      setErr(String(e));
    }
  }, []);

  useEffect(() => {
    load();
  }, [load]);

  async function save() {
    setMsg('');
    setErr('');
    try {
      await apiSend('PUT', '/v1/config/oidc', {
        issuer: form.issuer,
        audience: form.audience,
        jwks_url: form.jwks_url,
        admin_claim: form.admin_claim,
        admin_values: form.admin_values.split(',').map((v) => v.trim()).filter(Boolean),
      });
      setMsg('Saved. Restart the console to apply the new OIDC config.');
      await load();
    } catch (e) {
      setErr(`Save failed: ${e}`);
    }
  }

  return (
    <div>
      <h3>OIDC login config</h3>
      {cfg && !cfg.configured && <p className="kbc-muted">Not configured — the console is break-glass-only until this is set.</p>}
      {cfg?.updated_at && <p className="kbc-muted">Last updated {cfg.updated_at}.</p>}
      <div className="kbc-form">
        <input placeholder="issuer (https://idp…)" value={form.issuer} onChange={(e) => setForm({ ...form, issuer: e.target.value })} />
        <input placeholder="audience" value={form.audience} onChange={(e) => setForm({ ...form, audience: e.target.value })} />
        <input placeholder="jwks_url (https://…)" value={form.jwks_url} onChange={(e) => setForm({ ...form, jwks_url: e.target.value })} />
        <input placeholder="admin_claim (e.g. groups)" value={form.admin_claim} onChange={(e) => setForm({ ...form, admin_claim: e.target.value })} />
        <input placeholder="admin_values (comma-separated)" value={form.admin_values} onChange={(e) => setForm({ ...form, admin_values: e.target.value })} />
        <button
          onClick={save}
          disabled={!form.issuer || !form.audience || !form.jwks_url || !form.admin_claim || !form.admin_values}
        >
          Save
        </button>
      </div>
      {msg && <p className="kbc-notice">{msg}</p>}
      {err && <p className="kbc-error">{err}</p>}
    </div>
  );
}

export default function Accounts() {
  const [enrollments, setEnrollments] = useState<Enrollment[]>([]);
  const [scopes, setScopes] = useState<ScopeRow[]>([]);
  const [err, setErr] = useState('');
  const [loading, setLoading] = useState(true);
  // Track every in-flight revoke independently so concurrent revokes on
  // different rows don't clobber each other's disabled/busy state.
  const [busy, setBusy] = useState<Set<number>>(new Set());

  // apiGet/apiSend are stable module-level imports, so refresh has no reactive deps.
  const refresh = useCallback(async () => {
    // Each table updates independently — one failing endpoint must not blank the
    // other's (already-good) data.
    const [e, s] = await Promise.allSettled([
      apiGet<{ enrollments: Enrollment[] }>('/v1/enrollments'),
      apiGet<{ scopes: ScopeRow[] }>('/v1/scopes'),
    ]);
    const errs: string[] = [];
    if (e.status === 'fulfilled') setEnrollments(e.value.enrollments ?? []);
    else errs.push(`enrollments: ${e.reason}`);
    if (s.status === 'fulfilled') setScopes(s.value.scopes ?? []);
    else errs.push(`scopes: ${s.reason}`);
    setErr(errs.join('; '));
    setLoading(false);
  }, []);

  useEffect(() => {
    refresh();
  }, [refresh]);

  async function revoke(id: number) {
    if (busy.has(id)) return;
    if (!window.confirm(`Revoke enrollment #${id}? The client certificate stops working immediately.`))
      return;
    setBusy((b) => new Set(b).add(id));
    try {
      await apiSend('POST', `/v1/enrollments/${id}/revoke`);
      await refresh();
    } catch (e) {
      setErr(`revoke failed: ${e}`);
    } finally {
      setBusy((b) => {
        const n = new Set(b);
        n.delete(id);
        return n;
      });
    }
  }

  return (
    <section>
      <header className="kbc-dash-head">
        <h2>Accounts</h2>
        <button onClick={refresh}>Refresh</button>
      </header>
      {err && <p className="kbc-error">{err}</p>}
      {loading && <p className="kbc-muted">Loading…</p>}

      <h3>Scopes</h3>
      {scopes.length === 0 ? (
        <p className="kbc-muted">No scopes yet.</p>
      ) : (
        <table className="kbc-table">
          <thead>
            <tr>
              <th>Scope</th>
              <th>Certs</th>
              <th>Active</th>
            </tr>
          </thead>
          <tbody>
            {scopes.map((s) => (
              <tr key={s.scope}>
                <td>{s.scope}</td>
                <td>{s.count}</td>
                <td>{s.active}</td>
              </tr>
            ))}
          </tbody>
        </table>
      )}

      <h3>Enrollments</h3>
      {enrollments.length === 0 ? (
        <p className="kbc-muted">No issued certificates.</p>
      ) : (
        <table className="kbc-table">
          <thead>
            <tr>
              <th>ID</th>
              <th>Scope</th>
              <th>Fingerprint</th>
              <th>State</th>
              <th>Issued</th>
              <th>Last seen</th>
              <th></th>
            </tr>
          </thead>
          <tbody>
            {enrollments.map((e) => (
              <tr key={e.id} className={e.state === 'revoked' ? 'kbc-row-revoked' : ''}>
                <td>{e.id}</td>
                <td>
                  {e.scope || '(owner)'}
                  {e.legacy && <span className="kbc-badge kbc-badge-legacy">legacy</span>}
                </td>
                <td title={e.fingerprint}>
                  <code>{e.fingerprint.slice(0, 12)}…</code>
                </td>
                <td>
                  <span className={`kbc-badge ${e.state === 'revoked' ? 'kbc-badge-err' : 'kbc-badge-ok'}`}>
                    {e.state}
                  </span>
                </td>
                <td>{e.issued_at}</td>
                <td>{e.last_seen_at || '—'}</td>
                <td>
                  {e.state !== 'revoked' && (
                    <button disabled={busy.has(e.id)} onClick={() => revoke(e.id)}>
                      {busy.has(e.id) ? 'Revoking…' : 'Revoke'}
                    </button>
                  )}
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      )}

      <OidcEditor />
    </section>
  );
}
