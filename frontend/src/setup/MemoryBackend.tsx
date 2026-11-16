import { useState } from 'react';
import { Button } from '@rakuensoftware/smoothgui';
import { saveConfigValue, type ConfigMap } from './configApi';

export default function MemoryBackend({ config, onSaved, fetchImpl = fetch }: {
  config: ConfigMap; onSaved: (restartKeys: string[]) => void; fetchImpl?: typeof fetch;
}) {
  const configured = String(config.memory_backend_engine ?? '').trim();
  const retired = configured !== '' && configured !== 'native' && configured !== 'aimee-native';
  // Keep an unsupported saved selection visible until the administrator explicitly changes it.
  const [engine, setEngine] = useState(retired ? configured : 'native');
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState('');
  async function save() {
    if (engine !== 'native') return;
    setError(''); setSaving(true);
    try {
      const result = await saveConfigValue('memory.backend', {
        engine: 'native', endpoint: '', auth: 'bearer',
      }, { fetchImpl });
      if (!result.ok) { setError(result.error ?? 'Could not save the memory backend.'); return; }
      onSaved(['memory.backend']);
    } finally { setSaving(false); }
  }
  return <div style={{ display: 'grid', gap: 12 }}>
    <p>Aimee native memory uses the existing PostgreSQL memory store.</p>
    {retired && <p role="alert">The configured backend “{configured}” is no longer supported. Export and migrate its data before selecting Aimee native memory. Saving a selection does not migrate or delete existing data.</p>}
    <label>Memory backend <select aria-label="Memory backend" value={engine} disabled={saving} onChange={e => { setEngine(e.target.value); setError(''); }}>
      {retired && <option value={configured} disabled>{configured} (retired)</option>}
      <option value="native">Aimee native memory</option>
    </select></label>
    <p>Restart Aimee after saving to apply this choice. Deployment environment settings take precedence. A shared knowledge base keeps its own backend configuration.</p>
    {error && <p role="alert">{error}</p>}
    <Button variant="primary" disabled={saving || engine !== 'native'} onClick={save}>{saving ? 'Saving…' : 'Save memory backend'}</Button>
  </div>;
}
