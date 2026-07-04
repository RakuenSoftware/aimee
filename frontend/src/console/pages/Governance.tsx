import { useCallback, useEffect, useRef, useState } from 'react';
import { apiGet, apiSend, ApiError } from '../api';

// S5 Governance UI over the S4 backend: a decision-records browser + authoring
// form (surfacing the one-active-per-scope 409), and the policy-verdict action
// audit feed (mandatory time-window). The curator review queue is a separate
// curator-scoped credential and is deferred.

interface Decision {
  id: number;
  subject: string;
  options: string;
  chosen: string;
  rationale: string;
  outcome: string;
  status: string;
  revisit_when: string;
  supersedes_id: number;
  author: string;
  linked_policy_id: number;
  created_at: string;
}

interface AuditAction {
  id: string;
  target_surface: string;
  target_id: string;
  operator_id: string;
  scope_kind: string;
  scope_id: string;
  applied_at: string;
  applied_confidence: number;
  flagged_for_review: boolean;
}

export default function Governance() {
  return (
    <section>
      <h2>Governance</h2>
      <Decisions />
      <AuditFeed />
    </section>
  );
}

function Decisions() {
  const [decisions, setDecisions] = useState<Decision[]>([]);
  const [statusFilter, setStatusFilter] = useState('');
  const [err, setErr] = useState('');
  const [loading, setLoading] = useState(true);
  const [form, setForm] = useState({ subject: '', options: '', chosen: '', rationale: '' });
  const [notice, setNotice] = useState('');
  const [busy, setBusy] = useState(false);
  const seq = useRef(0);

  const refresh = useCallback(async () => {
    const my = ++seq.current;
    setLoading(true);
    try {
      const q = statusFilter ? `?status=${encodeURIComponent(statusFilter)}` : '';
      const r = await apiGet<{ decisions: Decision[] }>(`/v1/decisions${q}`);
      if (my !== seq.current) return; // a newer request superseded this one
      setDecisions(r.decisions ?? []);
      setErr('');
    } catch (e) {
      if (my === seq.current) setErr(String(e));
    } finally {
      if (my === seq.current) setLoading(false);
    }
  }, [statusFilter]);

  useEffect(() => {
    refresh();
  }, [refresh]);

  async function create() {
    setNotice('');
    setBusy(true);
    try {
      await apiSend('POST', '/v1/decisions', {
        subject: form.subject,
        options: form.options,
        chosen: form.chosen,
        rationale: form.rationale,
      });
      setForm({ subject: '', options: '', chosen: '', rationale: '' });
      setNotice('Decision recorded.');
      await refresh();
    } catch (e) {
      const conflict = e instanceof ApiError && e.status === 409;
      setNotice(
        conflict
          ? 'Conflict: an active decision already exists for this scope — supersede it instead.'
          : `Create failed: ${e}`,
      );
    } finally {
      setBusy(false);
    }
  }

  async function supersede(d: Decision) {
    const chosen = window.prompt(`Supersede #${d.id} (${d.subject}) — new chosen option:`, d.chosen);
    if (!chosen) return;
    try {
      await apiSend('POST', `/v1/decisions/${d.id}/supersede`, {
        subject: d.subject,
        options: d.options,
        chosen,
        linked_policy_id: d.linked_policy_id,
      });
      setNotice(`Superseded #${d.id}.`);
      await refresh();
    } catch (e) {
      setNotice(`Supersede failed: ${e}`);
    }
  }

  const canCreate = form.subject && form.options && form.chosen && !busy;

  return (
    <div>
      <h3>Decision records</h3>
      {err && <p className="kbc-error">{err}</p>}

      <div className="kbc-form">
        <input
          placeholder="subject (scope key)"
          value={form.subject}
          onChange={(e) => setForm({ ...form, subject: e.target.value })}
        />
        <input
          placeholder="options (e.g. a|b|c)"
          value={form.options}
          onChange={(e) => setForm({ ...form, options: e.target.value })}
        />
        <input
          placeholder="chosen"
          value={form.chosen}
          onChange={(e) => setForm({ ...form, chosen: e.target.value })}
        />
        <input
          placeholder="rationale"
          value={form.rationale}
          onChange={(e) => setForm({ ...form, rationale: e.target.value })}
        />
        <button disabled={!canCreate} onClick={create}>
          {busy ? 'Recording…' : 'Record decision'}
        </button>
      </div>
      {notice && <p className="kbc-notice">{notice}</p>}

      <div className="kbc-filter">
        <label>
          Status:
          <select value={statusFilter} onChange={(e) => setStatusFilter(e.target.value)}>
            <option value="">all</option>
            <option value="active">active</option>
            <option value="superseded">superseded</option>
            <option value="revisit_due">revisit_due</option>
          </select>
        </label>
      </div>

      {loading ? (
        <p className="kbc-muted">Loading…</p>
      ) : decisions.length === 0 ? (
        <p className="kbc-muted">No decisions.</p>
      ) : (
        <table className="kbc-table">
          <thead>
            <tr>
              <th>ID</th>
              <th>Subject</th>
              <th>Chosen</th>
              <th>Status</th>
              <th>Outcome</th>
              <th>Created</th>
              <th></th>
            </tr>
          </thead>
          <tbody>
            {decisions.map((d) => (
              <tr key={d.id} className={d.status === 'superseded' ? 'kbc-row-superseded' : ''}>
                <td>{d.id}</td>
                <td title={d.rationale}>{d.subject}</td>
                <td>{d.chosen}</td>
                <td>
                  <span className="kbc-badge">{d.status}</span>
                  {d.supersedes_id > 0 && <span className="kbc-muted"> ← #{d.supersedes_id}</span>}
                </td>
                <td>{d.outcome || '—'}</td>
                <td>{d.created_at}</td>
                <td>
                  {d.status === 'active' && <button onClick={() => supersede(d)}>Supersede</button>}
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      )}
    </div>
  );
}

function isoDaysAgo(days: number): string {
  // A UTC YYYY-MM-DD bound (no date lib); the server compares against text
  // 'YYYY-MM-DD HH:MI:SS', for which this date prefix is a valid lower bound.
  const ms = Date.now() - days * 86400000;
  return new Date(ms).toISOString().slice(0, 10);
}

function AuditFeed() {
  // Draft filter inputs; the query is applied only on Load (or first mount), so
  // typing a scope does not spam requests.
  const [since, setSince] = useState(isoDaysAgo(7));
  const [until, setUntil] = useState('');
  const [scopeKind, setScopeKind] = useState('');
  const [actions, setActions] = useState<AuditAction[]>([]);
  const [err, setErr] = useState('');
  const [loading, setLoading] = useState(false);
  const seq = useRef(0);

  const load = useCallback(async () => {
    if (!since) {
      setErr('A "since" bound is required.');
      setActions([]); // don't leave stale rows under an invalid query
      return;
    }
    const my = ++seq.current;
    setLoading(true);
    try {
      const params = new URLSearchParams({ since });
      if (until) params.set('until', until);
      if (scopeKind) params.set('scope_kind', scopeKind);
      const r = await apiGet<{ actions: AuditAction[] }>(`/v1/audit/actions?${params.toString()}`);
      if (my !== seq.current) return;
      setActions(r.actions ?? []);
      setErr('');
    } catch (e) {
      if (my === seq.current) setErr(String(e));
    } finally {
      if (my === seq.current) setLoading(false);
    }
  }, [since, until, scopeKind]);

  // Initial load only; subsequent loads are Load-button driven.
  const didMount = useRef(false);
  useEffect(() => {
    if (didMount.current) return;
    didMount.current = true;
    load();
  }, [load]);

  return (
    <div>
      <h3>Action audit</h3>
      <div className="kbc-filter">
        <label>
          Since <input type="date" value={since} onChange={(e) => setSince(e.target.value)} />
        </label>
        <label>
          Until <input type="date" value={until} onChange={(e) => setUntil(e.target.value)} />
        </label>
        <label>
          Scope
          <input
            placeholder="scope_kind"
            value={scopeKind}
            onChange={(e) => setScopeKind(e.target.value)}
          />
        </label>
        <button onClick={load}>Load</button>
      </div>
      {err && <p className="kbc-error">{err}</p>}
      {loading ? (
        <p className="kbc-muted">Loading…</p>
      ) : actions.length === 0 ? (
        <p className="kbc-muted">No audited actions in this window.</p>
      ) : (
        <table className="kbc-table">
          <thead>
            <tr>
              <th>When</th>
              <th>Surface</th>
              <th>Target</th>
              <th>Operator</th>
              <th>Scope</th>
              <th>Conf.</th>
              <th>Flagged</th>
            </tr>
          </thead>
          <tbody>
            {actions.map((a) => (
              <tr key={a.id} className={a.flagged_for_review ? 'kbc-row-flagged' : ''}>
                <td>{a.applied_at}</td>
                <td>{a.target_surface}</td>
                <td>{a.target_id}</td>
                <td>{a.operator_id || '—'}</td>
                <td>
                  {a.scope_kind}
                  {a.scope_id ? `:${a.scope_id}` : ''}
                </td>
                <td>{a.applied_confidence.toFixed(2)}</td>
                <td>{a.flagged_for_review ? '⚑' : ''}</td>
              </tr>
            ))}
          </tbody>
        </table>
      )}
    </div>
  );
}
