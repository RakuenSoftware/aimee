import { useState } from 'react';
import { SettingsMenu } from '@rakuensoftware/smoothgui';
import type { SettingField } from '@rakuensoftware/smoothgui';

/* A gear button in the top bar that opens a dropdown of aimee runtime settings
 * (autonomous mode, etc.). The dropdown UI is the generic smoothgui
 * <SettingsMenu>; this adapter supplies the aimee-specific data + persistence
 * (the allowlisted server config via /api/settings). Changes take effect on the
 * next turn. */

export default function SettingsPanel() {
  const [fields, setFields] = useState<SettingField[]>([]);
  const [busy, setBusy] = useState('');
  const [err, setErr] = useState('');
  const [loaded, setLoaded] = useState(false);

  function load() {
    if (loaded) return;
    fetch('/api/settings', { headers: { 'X-CSRF-Token': window._csrf || '' } })
      .then(r => r.json())
      .then((d: { fields?: SettingField[] }) => { setFields(d.fields || []); setLoaded(true); })
      .catch(() => setErr('Failed to load settings'));
  }

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
    <SettingsMenu
      title="Runtime settings"
      fields={fields}
      onChange={setField}
      onOpen={load}
      loading={!loaded}
      error={err}
      busyKey={busy}
    />
  );
}
