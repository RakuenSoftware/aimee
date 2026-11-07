import { useState } from 'react';
import { apiSendRaw } from '../api';

const examples = {
  head: { operation: 'ontology_head', graph: '<native graph UUID>' },
  migrations: { operation: 'ontology_migrations' },
  corrections: { operation: 'correction_list', scope: { kind: 'user', principal: { kind: '<identity scheme>', identity: '<User identity>', verified_by: '<authority node UUID>' } }, limit: 20, max_work: 10000, max_bytes: 65536 },
};

export default function NativeMemory() {
  const [owner, setOwner] = useState('');
  const [request, setRequest] = useState(JSON.stringify(examples.head, null, 2));
  const [busy, setBusy] = useState(false);
  const [status, setStatus] = useState<number | null>(null);
  const [response, setResponse] = useState('');
  const [error, setError] = useState('');

  async function submit() {
    setError('');
    if (!/^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$/.test(owner)) {
      setError('Enter the exact native database owner UUID.');
      return;
    }
    try {
      // Syntax checking never supplies the payload or formats native identities.
      const syntax: unknown = JSON.parse(request);
      if (!syntax || typeof syntax !== 'object' || Array.isArray(syntax)) throw new Error('A JSON request object is required.');
    } catch {
      setError('Enter a valid JSON request object.');
      return;
    }
    setBusy(true);
    setStatus(null);
    setResponse('');
    try {
      const body = '{"owner":' + JSON.stringify(owner) + ',"request":' + request + '}';
      const result = await apiSendRaw('POST', '/v1/console/memory', body);
      setStatus(result.status);
      setResponse(result.text);
    } catch (failure) {
      setError(String(failure));
    } finally {
      setBusy(false);
    }
  }

  return (
    <section>
      <h1>Memory administration</h1>
      <p>Inspect native ontology and corrections, or submit an explicit native administration request. The service checks your installed Operator authority and current permissions.</p>
      <p>Use native owner, graph and version identities. Legacy SQL row and commit IDs belong to the separate legacy pages.</p>
      <label>Native database owner UUID<br /><input aria-label="Native database owner UUID" value={owner} onChange={(event) => setOwner(event.target.value)} style={{ width: 'min(100%, 42rem)' }} /></label>
      <p>Read-only request examples:</p>
      <div style={{ display: 'flex', gap: 8 }}>
        <button disabled={busy} onClick={() => setRequest(JSON.stringify(examples.head, null, 2))}>Ontology head</button>
        <button disabled={busy} onClick={() => setRequest(JSON.stringify(examples.migrations, null, 2))}>Ontology migrations</button>
        <button disabled={busy} onClick={() => setRequest(JSON.stringify(examples.corrections, null, 2))}>Correction list</button>
      </div>
      <label>Native request JSON<br /><textarea aria-label="Native request JSON" rows={14} value={request} onChange={(event) => setRequest(event.target.value)} style={{ width: '100%', fontFamily: 'monospace' }} /></label>
      <p>The request and response preserve JSON number tokens exactly, including full-width version axes. Sending a mutation requires an explicit request; no operation runs automatically.</p>
      <button disabled={busy} onClick={() => void submit()}>{busy ? 'Sending…' : 'Send request'}</button>
      {error && <p role="alert" className="kbc-error">{error}</p>}
      {status !== null && <p role="status">HTTP {status}</p>}
      {response && <pre aria-label="Native response" style={{ whiteSpace: 'pre-wrap', overflowWrap: 'anywhere' }}>{response}</pre>}
    </section>
  );
}
