import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { useSessions } from '../SessionContext';
import { loadConfig, type ConfigMap } from '../setup/configApi';
import { computeReadiness, stepsRemaining } from '../setup/readiness';
import { requestOpenWizard, isDismissed, SETUP_UPDATED_EVENT } from '../setup/setupState';

/* Header chip: "Setup — N left". Reads GET /api/config once (and again whenever
 * the wizard reports a change or the window regains focus), computes readiness
 * against the active session's project, and shows how many required steps remain.
 * Hidden entirely once ready. Clicking it opens the wizard. On first load, if the
 * instance is not ready and the wizard hasn't been dismissed, it auto-opens the
 * wizard once. All heavy lifting is in the tested setup/ modules. */

export default function SetupChip() {
  const { active } = useSessions();
  const hasProject = !!active?.projectName;
  const [cfg, setCfg] = useState<ConfigMap | null>(null);

  const load = useCallback(() => {
    loadConfig().then(setCfg);
  }, []);

  useEffect(() => { load(); }, [load]);

  useEffect(() => {
    const onUpdate = () => load();
    window.addEventListener(SETUP_UPDATED_EVENT, onUpdate);
    window.addEventListener('focus', onUpdate);
    return () => {
      window.removeEventListener(SETUP_UPDATED_EVENT, onUpdate);
      window.removeEventListener('focus', onUpdate);
    };
  }, [load]);

  const readiness = useMemo(
    () => (cfg ? computeReadiness(cfg, hasProject) : null),
    [cfg, hasProject],
  );

  // Auto-open the wizard once per page load when unconfigured and not dismissed.
  const autoOpened = useRef(false);
  useEffect(() => {
    if (readiness && !readiness.ready && !isDismissed() && !autoOpened.current) {
      autoOpened.current = true;
      requestOpenWizard();
    }
  }, [readiness]);

  if (!readiness || readiness.ready) return null;

  const n = stepsRemaining(readiness);
  return (
    <button
      onClick={requestOpenWizard}
      title="Finish setting up this instance"
      style={{
        display: 'flex', alignItems: 'center', gap: 6, padding: '3px 10px',
        borderRadius: 12, cursor: 'pointer', fontSize: 12.5, whiteSpace: 'nowrap',
        background: '#3a2a12', color: '#f4b860', border: '1px solid #6a4a1a',
      }}
    >
      <span aria-hidden>⚙️</span> Setup — {n} left
    </button>
  );
}
