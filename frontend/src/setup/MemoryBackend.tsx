import { useState } from 'react';
import { Button } from '@rakuensoftware/smoothgui';
import { saveConfigValue, type ConfigMap } from './configApi';

type Engine = 'native' | 'cognee' | 'hillock';
import { validateMemoryEndpoint } from './memoryBackendConfig';
export { validateMemoryEndpoint } from './memoryBackendConfig';

export default function MemoryBackend({ config, onSaved, fetchImpl = fetch }: {
  config: ConfigMap; onSaved: (restartKeys: string[]) => void; fetchImpl?: typeof fetch;
}) {
  const configured = config.memory_backend_engine;
  const [engine, setEngine] = useState<Engine>(configured === 'cognee' || configured === 'hillock' ? configured : 'native');
  const [endpoint, setEndpoint] = useState(String(config.memory_backend_endpoint ?? ''));
  const [auth, setAuth] = useState(String(config.memory_backend_auth ?? 'bearer'));
  const [token, setToken] = useState('');
  const [credentialStored, setCredentialStored] = useState(config.memory_backend_token === true);
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState('');
  const external = engine !== 'native';
  async function save() {
    setError('');
    if (external && !validateMemoryEndpoint(endpoint.trim())) { setError('Enter the HTTP(S) origin of your running memory service, without a path or credentials.'); return; }
    if (external && auth !== 'none' && !token.trim() && !credentialStored) { setError('Enter the service credential to store in the Vault.'); return; }
    setSaving(true);
    try {
      // Seal the credential first. Failure leaves the currently selected backend intact.
      if (external && auth !== 'none' && token.trim()) {
        const result = await saveConfigValue('memory_backend_token', token.trim(), { fetchImpl });
        if (!result.ok) { setError(result.error ?? 'Could not store the credential.'); return; }
        setToken(''); setCredentialStored(true);
      }
      const result = await saveConfigValue('memory.backend', {
        engine, endpoint: external ? endpoint.trim() : '', auth: external ? auth : 'bearer',
      }, { fetchImpl });
      if (!result.ok) { setError(result.error ?? 'Could not save the memory backend.'); return; }
      onSaved(['memory.backend']);
    } finally { setSaving(false); }
  }
  return <div style={{ display: 'grid', gap: 12 }}>
    <p>Choose where Aimee keeps and retrieves your personal memory. Aimee native uses the existing PostgreSQL memory store. Cognee and Hillock use separate durable catalogs with their own retrieval service.</p>
    <label>Memory backend <select aria-label="Memory backend" value={engine} disabled={saving} onChange={e => {
      setEngine(e.target.value as Engine); setCredentialStored(e.target.value === configured && config.memory_backend_token === true); setEndpoint(''); setAuth('bearer'); setToken(''); setError('');
    }}>
      <option value="native">Aimee native memory</option><option value="cognee">Cognee</option><option value="hillock">Hillock</option>
    </select></label>
    {external && <>
      <p>Start your {engine === 'hillock' ? 'supported Hillock sidecar' : 'Cognee service'} first. Use an address reachable from the Aimee server. Selecting a backend does not install the service or migrate existing memories.</p>
      <label>Service origin <input aria-label="Memory service origin" value={endpoint} disabled={saving} onChange={e => setEndpoint(e.target.value)} placeholder={engine === 'hillock' ? 'http://hillock:8097' : 'http://cognee:8000'} /></label>
      <label>Authentication <select aria-label="Memory service authentication" value={auth} disabled={saving} onChange={e => setAuth(e.target.value)}>
        <option value="bearer">Bearer token</option>{engine === 'cognee' && <option value="api-key">API key</option>}<option value="none">None — isolated development service</option>
      </select></label>
      {auth !== 'none' && <label>Service credential <input aria-label="Memory service credential" type="password" autoComplete="new-password" value={token} disabled={saving} onChange={e => setToken(e.target.value)} placeholder={config.memory_backend_token === true ? 'Credential already stored; leave blank to retain it' : 'Stored only in the Vault'} /></label>}
      <p>The configured service must pass startup readiness checks; service failures are reported without switching to native memory.</p>
    </>}
    <p>Restart Aimee after saving to apply this choice. Deployment environment settings take precedence. A shared knowledge base keeps its own backend configuration.</p>
    {error && <p role="alert">{error}</p>}
    <Button variant="primary" disabled={saving} onClick={save}>{saving ? 'Saving…' : 'Save memory backend'}</Button>
  </div>;
}
