import { useEffect, useRef, useState } from 'react';

/* A gear button in the top bar that opens a dropdown of aimee runtime settings
 * (autonomous mode, etc.). Reads/writes the allowlisted server config via
 * /api/settings; changes take effect on the next turn. */

interface Field {
  key: string;
  label: string;
  type: string; // "bool" | "int"
  help?: string;
  value: unknown;
}

export default function SettingsPanel() {
  const [open, setOpen] = useState(false);
  const [fields, setFields] = useState<Field[]>([]);
  const [busy, setBusy] = useState('');
  const [err, setErr] = useState('');
  const [loaded, setLoaded] = useState(false);
  const ref = useRef<HTMLDivElement>(null);

  function load() {
    fetch('/api/settings', { headers: { 'X-CSRF-Token': window._csrf || '' } })
      .then(r => r.json())
      .then((d: { fields?: Field[] }) => { setFields(d.fields || []); setLoaded(true); })
      .catch(() => setErr('Failed to load settings'));
  }

  useEffect(() => { if (open && !loaded) load(); }, [open, loaded]);

  // Close on outside click / Escape.
  useEffect(() => {
    if (!open) return;
    const onDoc = (e: MouseEvent) => { if (ref.current && !ref.current.contains(e.target as Node)) setOpen(false); };
    const onKey = (e: KeyboardEvent) => { if (e.key === 'Escape') setOpen(false); };
    document.addEventListener('mousedown', onDoc);
    document.addEventListener('keydown', onKey);
    return () => { document.removeEventListener('mousedown', onDoc); document.removeEventListener('keydown', onKey); };
  }, [open]);

  async function setField(key: string, value: unknown) {
    setBusy(key); setErr('');
    const prev = fields;
    setFields(p => p.map(f => (f.key === key ? { ...f, value } : f))); // optimistic
    try {
      const r = await fetch('/api/settings', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': window._csrf || '' },
        body: JSON.stringify({ key, value }),
      });
      if (!r.ok) { setErr(`Failed to set ${key}`); setFields(prev); }
    } catch {
      setErr(`Failed to set ${key}`); setFields(prev);
    } finally {
      setBusy('');
    }
  }

  return (
    <div ref={ref} style={{ position: 'relative' }}>
      <button
        onClick={() => setOpen(o => !o)}
        title="Runtime settings"
        style={{ background: 'transparent', border: 'none', color: open ? '#8cf' : '#aab', cursor: 'pointer', fontSize: 18, padding: '0 4px', lineHeight: 1 }}
      >
        ⚙
      </button>
      {open && (
        <div
          style={{
            position: 'absolute', top: '100%', right: 0, marginTop: 8, width: 300, zIndex: 50,
            background: '#1a1a28', border: '1px solid #2a2a3a', borderRadius: 8, padding: '12px 14px',
            boxShadow: '0 8px 24px rgba(0,0,0,0.45)',
          }}
        >
          <div style={{ fontWeight: 600, fontSize: 13, marginBottom: 8, color: '#cde' }}>Runtime settings</div>
          {err && <div style={{ color: '#e88', fontSize: 12, marginBottom: 6 }}>{err}</div>}
          {!loaded && <div style={{ color: '#889', fontSize: 12 }}>Loading…</div>}
          {loaded && fields.length === 0 && <div style={{ color: '#889', fontSize: 12 }}>No settings available.</div>}
          {fields.map(f => (
            <div key={f.key} style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', gap: 10, padding: '6px 0' }}>
              <div style={{ minWidth: 0 }}>
                <div style={{ fontSize: 13, color: '#dde' }}>{f.label}</div>
                {f.help && <div style={{ fontSize: 11, color: '#778' }}>{f.help}</div>}
              </div>
              {f.type === 'bool' ? (
                <input
                  type="checkbox"
                  checked={!!f.value}
                  disabled={busy === f.key}
                  onChange={e => setField(f.key, e.target.checked)}
                  style={{ width: 16, height: 16, flexShrink: 0, cursor: 'pointer' }}
                />
              ) : (
                <input
                  type="number"
                  value={typeof f.value === 'number' ? f.value : 0}
                  disabled={busy === f.key}
                  onChange={e => setField(f.key, parseInt(e.target.value || '0', 10) || 0)}
                  style={{ width: 64, flexShrink: 0, background: '#13131f', color: '#cde', border: '1px solid #3a3a55', borderRadius: 4, padding: '2px 4px', fontSize: 13 }}
                />
              )}
            </div>
          ))}
        </div>
      )}
    </div>
  );
}
