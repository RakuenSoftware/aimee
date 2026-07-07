import { useEffect, useState } from 'react';
import { SettingsMenu } from '@rakuensoftware/smoothgui';
import type { SettingField } from '@rakuensoftware/smoothgui';

/* A gear button in the top bar that opens a dropdown of aimee runtime settings
 * (autonomous mode, etc.). Bool/int fields render via the generic smoothgui
 * <SettingsMenu>; enum fields (e.g. the KB retrieval fusion mode) render here as
 * a native <select> so the choice works regardless of the smoothgui version.
 * This adapter supplies the aimee-specific data + persistence (the allowlisted
 * server config via /api/settings). Changes take effect on the next turn. */

type AimeeField = {
  key: string;
  label: string;
  type: string;
  help?: string;
  options?: string[];
  value?: unknown;
};

export default function SettingsPanel() {
  const [fields, setFields] = useState<AimeeField[]>([]);
  const [busy, setBusy] = useState('');
  const [err, setErr] = useState('');
  const [loaded, setLoaded] = useState(false);

  function load() {
    if (loaded) return;
    fetch('/api/settings', { headers: { 'X-CSRF-Token': window._csrf || '' } })
      .then(r => r.json())
      .then((d: { fields?: AimeeField[] }) => { setFields(d.fields || []); setLoaded(true); })
      .catch(() => setErr('Failed to load settings'));
  }

  // Load once on mount so enum controls are populated without opening the menu.
  useEffect(load, []); // eslint-disable-line react-hooks/exhaustive-deps

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

  const enumFields = fields.filter(f => f.type === 'enum');
  const menuFields = fields.filter(f => f.type !== 'enum');

  return (
    <div className="aimee-settings">
      <SettingsMenu
        title="Runtime settings"
        fields={menuFields as unknown as SettingField[]}
        onChange={setField}
        onOpen={load}
        loading={!loaded}
        error={err}
        busyKey={busy}
      />
      {enumFields.map(f => (
        <label key={f.key} className="aimee-setting-enum" title={f.help}>
          <span>{f.label}</span>
          <select
            value={String(f.value ?? '')}
            disabled={busy === f.key}
            onChange={e => setField(f.key, e.target.value)}
          >
            {(f.options ?? []).map(o => (
              <option key={o} value={o}>{o}</option>
            ))}
          </select>
        </label>
      ))}
    </div>
  );
}
