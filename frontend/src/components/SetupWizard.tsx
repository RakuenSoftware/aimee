import { useCallback, useEffect, useMemo, useState } from 'react';
import { useToast } from '@rakuensoftware/smoothgui';
import { useSessions } from '../SessionContext';
import { loadConfig, saveConfigValue, type ConfigMap } from '../setup/configApi';
import { visibleSteps, isRestartKey, helpFor, type WizardKbMode } from '../setup/wizardSteps';
import { computeReadiness } from '../setup/readiness';
import { setDismissed, notifySetupUpdated } from '../setup/setupState';
import PrimaryChooser from '../setup/PrimaryChooser';
import KnowledgeBase from '../setup/KnowledgeBase';
import DeployTopology from '../setup/DeployTopology';
import ConnectHosts from '../setup/ConnectHosts';
import ConnectWorkspace from '../setup/ConnectWorkspace';
import type { KbMode } from '../setup/deployTopology';

/* First-run setup wizard. A modal over the app that walks the operator through the
 * minimum path to a working turn. It forks at the Knowledge-base step: a remote KB
 * hides the deploy-topology + shared-store steps (visibleSteps() reflects the live
 * kb_mode). Every value is written through the existing POST /api/config/set
 * allowlist and the /api/git/* routes (no new config backend). Non-blocking:
 * closable at any time via ×, Esc, the backdrop, or "Later".
 *
 * Design notes:
 * - All config I/O + step data live in the tested setup/ modules; this file is
 *   presentation + local step state.
 * - Every generic-key save is guarded: a 4xx/5xx/network failure surfaces a Toast
 *   and keeps the operator on the step with their input intact.
 * - Keys carrying RESTART_KEYS are tracked into `pendingRestart` and listed on the
 *   summary, since they only take effect after a server restart. */

function humanize(key: string): string {
  const s = key.replace(/_/g, ' ').trim();
  return s.charAt(0).toUpperCase() + s.slice(1);
}

// Coerce a text input back to the field's type before saving: numeric fields
// (existing number value, or *_dim / *_port keys) go as numbers, everything else
// as a string. Keeps the server's typed allowlist happy.
function coerce(key: string, raw: string, original: unknown): unknown {
  if (typeof original === 'number' || /(_dim|_port)$/.test(key)) {
    const n = Number(raw);
    return Number.isFinite(n) ? n : raw;
  }
  return raw;
}

function csrf(): string {
  try {
    if (typeof window !== 'undefined') return (window as { _csrf?: string })._csrf || '';
  } catch {
    /* ignore */
  }
  return '';
}

// How many git hosts have a stored credential (drives the optional connection
// step's readiness). Network failure ⇒ 0; the step is optional so this never
// blocks "ready".
async function fetchHostCount(): Promise<number> {
  try {
    const r = await fetch('/api/git/credentials', { headers: { 'X-CSRF-Token': csrf() } });
    const d = await r.json();
    return r.ok && Array.isArray(d.hosts) ? d.hosts.length : 0;
  } catch {
    return 0;
  }
}

export default function SetupWizard({ open, onClose }: { open: boolean; onClose: () => void }) {
  const toast = useToast();
  const { active } = useSessions();
  const hasProject = !!active?.projectName;

  const [cfg, setCfg] = useState<ConfigMap>({});
  const [draft, setDraft] = useState<Record<string, string>>({});
  const [idx, setIdx] = useState(0);
  const [saving, setSaving] = useState(false);
  const [pendingRestart, setPendingRestart] = useState<string[]>([]);
  const [showSummary, setShowSummary] = useState(false);
  const [hostsConnected, setHostsConnected] = useState(0);

  const kbMode: WizardKbMode = String(cfg.kb_mode) === 'remote' ? 'remote' : 'local';
  const steps = useMemo(() => visibleSteps(kbMode), [kbMode]);
  const total = steps.length;
  // Clamp the cursor: switching to a remote KB shrinks the visible list.
  const safeIdx = Math.min(idx, total - 1);
  const step = steps[safeIdx];

  // (Re)load config each time the wizard opens; reset step + transient state.
  useEffect(() => {
    if (!open) return;
    setIdx(0);
    setShowSummary(false);
    setPendingRestart([]);
    loadConfig().then((c) => {
      setCfg(c);
      const d: Record<string, string> = {};
      for (const s of visibleSteps(String(c.kb_mode) === 'remote' ? 'remote' : 'local')) {
        for (const k of s.keys) {
          const v = c[k];
          d[k] = v == null ? '' : String(v);
        }
      }
      setDraft(d);
    });
    fetchHostCount().then(setHostsConnected);
  }, [open]);

  const close = useCallback(() => { onClose(); }, [onClose]);

  // Esc closes the wizard (non-blocking).
  useEffect(() => {
    if (!open) return;
    const onKey = (e: KeyboardEvent) => { if (e.key === 'Escape') close(); };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [open, close]);

  const readiness = useMemo(
    () => computeReadiness(cfg, hasProject, hostsConnected),
    [cfg, hasProject, hostsConnected],
  );

  if (!open) return null;

  function advance() {
    if (safeIdx < total - 1) setIdx(safeIdx + 1);
    else setShowSummary(true);
  }

  // The primary chooser configures the primary through the agent endpoints; once
  // it succeeds we stamp the legacy `provider` breadcrumb (which readiness + the
  // header chip read) and move on. A breadcrumb-save failure is non-fatal.
  async function handlePrimaryConfigured(provider: string) {
    const res = await saveConfigValue('provider', provider);
    setCfg((c) => ({ ...c, provider: res.ok ? res.value ?? provider : provider }));
    notifySetupUpdated();
    advance();
  }

  // The knowledge-base step records the local/remote choice (+ remote url/token).
  // Reload config so visibleSteps reflects the new kb_mode (remote hides the
  // deploy + DB2 steps), track restart-pending, advance.
  async function handleKbSaved(restartKeys: string[], _mode: KbMode) {
    const c = await loadConfig();
    setCfg(c);
    setPendingRestart((prev) => Array.from(new Set([...prev, ...restartKeys])));
    notifySetupUpdated();
    advance();
  }

  // The deploy-topology page writes its own config keys (per-role llm_*). It
  // reports the restart-class keys it changed; refresh cfg, track restart, advance.
  async function handleDeploySaved(restartKeys: string[]) {
    const c = await loadConfig();
    setCfg(c);
    setPendingRestart((prev) => Array.from(new Set([...prev, ...restartKeys])));
    notifySetupUpdated();
    advance();
  }

  // Save every edited key in the current (generic keyed) step. Any failure aborts
  // advance and Toasts; successes update the live cfg + restart tracking.
  async function saveStep() {
    setSaving(true);
    const savedCfg: ConfigMap = { ...cfg };
    const restart = new Set(pendingRestart);
    try {
      for (const key of step.keys) {
        const raw = draft[key] ?? '';
        // Skip keys the operator left exactly as they already were.
        const originalStr = cfg[key] == null ? '' : String(cfg[key]);
        if (raw === originalStr) continue;
        const value = coerce(key, raw, cfg[key]);
        const res = await saveConfigValue(key, value);
        if (!res.ok) {
          toast.error(`Couldn’t save ${humanize(key)}: ${res.error ?? 'unknown error'}`);
          setSaving(false);
          setCfg(savedCfg); // keep whatever succeeded so far
          setPendingRestart(Array.from(restart));
          return; // stay on the step, input preserved
        }
        savedCfg[key] = res.value ?? value;
        if (isRestartKey(key)) restart.add(key);
      }
      setCfg(savedCfg);
      setPendingRestart(Array.from(restart));
      notifySetupUpdated();
      setSaving(false);
      toast.success(`${step.title} saved`);
      advance();
    } catch (e) {
      setSaving(false);
      toast.error(e instanceof Error ? e.message : 'Save failed');
    }
  }

  const backdrop: React.CSSProperties = {
    position: 'fixed', inset: 0, zIndex: 1000, background: 'rgba(10,10,18,0.55)',
    display: 'flex', alignItems: 'center', justifyContent: 'center', padding: 16,
  };
  const card: React.CSSProperties = {
    width: 'min(560px, 100%)', maxHeight: '86vh', overflow: 'auto', background: '#fff',
    borderRadius: 12, border: '1px solid #dde', boxShadow: '0 12px 40px rgba(0,0,0,0.3)',
    padding: '20px 22px', fontFamily: 'system-ui', color: '#233',
  };
  const primaryBtn: React.CSSProperties = {
    padding: '7px 16px', borderRadius: 7, border: '1px solid #2c6', background: '#2c8f56',
    color: '#fff', cursor: 'pointer', fontSize: 13.5, fontWeight: 600,
  };
  const ghostBtn: React.CSSProperties = {
    padding: '7px 14px', borderRadius: 7, border: '1px solid #ccd', background: '#f4f6fb',
    color: '#446', cursor: 'pointer', fontSize: 13,
  };
  const input: React.CSSProperties = {
    width: '100%', boxSizing: 'border-box', padding: '7px 9px', borderRadius: 6,
    border: '1px solid #ccd', fontSize: 13, fontFamily: 'ui-monospace, monospace',
  };

  const stepNum = safeIdx + 1;

  return (
    <div style={backdrop} onClick={close}>
      <div style={card} role="dialog" aria-label="Setup wizard" onClick={(e) => e.stopPropagation()}>
        <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginBottom: 6 }}>
          <strong style={{ fontSize: 17 }}>Set up this instance</strong>
          <button aria-label="Close setup" title="Close" onClick={close}
            style={{ background: 'none', border: 'none', color: '#9aa', cursor: 'pointer', fontSize: 20, lineHeight: 1 }}>×</button>
        </div>

        {showSummary ? (
          <div>
            <p style={{ fontSize: 13, color: '#556', margin: '4px 0 12px' }}>
              {readiness.ready ? 'Everything required is configured. 🎉' : 'Here’s what’s left:'}
            </p>
            <div style={{ display: 'grid', gap: 6, marginBottom: 14 }}>
              {(Object.entries(readiness.steps)).map(([id, s]) => (
                <div key={id} style={{ display: 'flex', alignItems: 'center', gap: 8, fontSize: 13 }}>
                  <span aria-hidden>{s.ok ? '✅' : s.optional ? '⚪' : '⛔'}</span>
                  <span style={{ fontWeight: 600, textTransform: 'capitalize', minWidth: 92 }}>{id.replace(/_/g, ' ')}</span>
                  <span style={{ color: '#667' }}>{s.detail}{s.optional && !s.ok ? ' (optional)' : ''}</span>
                </div>
              ))}
            </div>
            {pendingRestart.length > 0 && (
              <div style={{ fontSize: 12.5, color: '#8a5a00', background: '#fff6e6', border: '1px solid #f0d8a8', borderRadius: 6, padding: '8px 10px', marginBottom: 14 }}>
                ⏳ Restart required for: {pendingRestart.map(humanize).join(', ')} — these take effect after the server restarts.
              </div>
            )}
            <div style={{ display: 'flex', justifyContent: 'space-between' }}>
              <button style={ghostBtn} onClick={() => { setShowSummary(false); setIdx(0); }}>Back</button>
              <button style={primaryBtn} onClick={() => { setDismissed(true); notifySetupUpdated(); close(); }}>Finish</button>
            </div>
          </div>
        ) : (
          <div>
            <div style={{ fontSize: 12, color: '#8899aa', marginBottom: 4 }}>Step {stepNum} of {total}</div>
            <div style={{ fontSize: 15, fontWeight: 700, marginBottom: 10 }}>
              {step.title}{step.optional ? <span style={{ color: '#9aa', fontWeight: 400, fontSize: 12 }}> · optional</span> : null}
            </div>

            {step.kind === 'chooser' ? (
              <PrimaryChooser onConfigured={handlePrimaryConfigured} />
            ) : step.kind === 'kb' ? (
              <KnowledgeBase onSaved={handleKbSaved} />
            ) : step.kind === 'deploy' ? (
              <DeployTopology onSaved={handleDeploySaved} />
            ) : step.kind === 'connection' ? (
              <ConnectHosts onDone={advance} onHostsChanged={setHostsConnected} />
            ) : step.kind === 'workspace' ? (
              <ConnectWorkspace onDone={advance} />
            ) : step.keys.length > 0 ? (
              <div style={{ display: 'grid', gap: 12, marginBottom: 16 }}>
                {step.keys.map((key) => (
                  <label key={key} style={{ display: 'block' }}>
                    <div style={{ fontSize: 12.5, fontWeight: 600, marginBottom: 3 }}>
                      {humanize(key)}
                      {isRestartKey(key) ? <span style={{ color: '#8a5a00', fontWeight: 400 }}> · needs restart</span> : null}
                    </div>
                    {helpFor(key) && <div style={{ fontSize: 11.5, color: '#778', marginBottom: 4, lineHeight: 1.4 }}>{helpFor(key)}</div>}
                    <input
                      style={input}
                      value={draft[key] ?? ''}
                      onChange={(e) => setDraft((p) => ({ ...p, [key]: e.target.value }))}
                      placeholder={key}
                    />
                  </label>
                ))}
              </div>
            ) : null}

            <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between' }}>
              <div style={{ display: 'flex', gap: 8 }}>
                <button style={ghostBtn} disabled={safeIdx === 0} onClick={() => setIdx(Math.max(0, safeIdx - 1))}>Back</button>
                <button style={ghostBtn} onClick={close}>Later</button>
              </div>
              <div style={{ display: 'flex', gap: 8 }}>
                {step.optional && <button style={ghostBtn} onClick={advance}>Skip</button>}
                {step.keys.length > 0 ? (
                  <button style={primaryBtn} disabled={saving} onClick={saveStep}>{saving ? 'Saving…' : 'Save & continue'}</button>
                ) : step.kind === 'chooser' || step.kind === 'kb' || step.kind === 'deploy' || step.kind === 'connection' || step.kind === 'workspace' ? (
                  // Bespoke steps own their own primary action (they call advance()).
                  null
                ) : (
                  <button style={primaryBtn} onClick={advance}>{safeIdx === total - 1 ? 'Review' : 'Next'}</button>
                )}
              </div>
            </div>
          </div>
        )}
      </div>
    </div>
  );
}
