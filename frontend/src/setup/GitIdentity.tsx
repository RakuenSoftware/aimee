import { useState } from 'react';
import { Button, useToast } from '@rakuensoftware/smoothgui';

type FetchLike = typeof fetch;

function csrf(): string {
  try {
    return typeof window === 'undefined' ? '' : (window as { _csrf?: string })._csrf || '';
  } catch {
    return '';
  }
}

function safeError(value: unknown, fallback: string): string {
  if (typeof value !== 'string') return fallback;
  const text = value.replace(/[<>\u0000-\u001f\u007f]/g, ' ').trim().slice(0, 240);
  return text || fallback;
}

export async function storeIdentityField(cred: 'author_name' | 'author_email', secret: string,
                                         fetchImpl: FetchLike): Promise<string | null> {
  const token = csrf();
  if (!token) return 'security token unavailable; reload Setup and try again';
  const controller = new AbortController();
  const timeout = window.setTimeout(() => controller.abort(), 10_000);
  try {
    const response = await fetchImpl('/api/vault/credentials', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': token },
      body: JSON.stringify({ agent: 'git', cred, secret }),
      signal: controller.signal,
    });
    const data = await response.json().catch(() => ({})) as { error?: string };
    if (!response.ok || data.error) {
      return safeError(data.error, `server returned ${response.status}`);
    }
    return null;
  } catch (error) {
    if (controller.signal.aborted) return 'request timed out; try again';
    return safeError(error instanceof Error ? error.message : null, 'network error');
  } finally {
    window.clearTimeout(timeout);
  }
}

/** Required setup step for the server-principal Git author. Values go directly
 * to the Vault and are never returned to the browser or persisted as config. */
export default function GitIdentity({ onSaved, onSkip, fetchImpl = fetch }: {
  onSaved: () => void;
  onSkip: () => void;
  fetchImpl?: FetchLike;
}) {
  const toast = useToast();
  const [name, setName] = useState('');
  const [email, setEmail] = useState('');
  const [saving, setSaving] = useState(false);

  async function save() {
    const cleanName = name.trim();
    const cleanEmail = email.trim();
    if (!cleanName) {
      toast.error('Enter the name Git should put on commits.');
      return;
    }
    if (!/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(cleanEmail)) {
      toast.error('Enter a valid commit email address.');
      return;
    }
    setSaving(true);
    const nameError = await storeIdentityField('author_name', cleanName, fetchImpl);
    if (nameError) {
      setSaving(false);
      toast.error(`Couldn’t store the Git author name: ${nameError}`);
      return;
    }
    const emailError = await storeIdentityField('author_email', cleanEmail, fetchImpl);
    setSaving(false);
    if (emailError) {
      toast.error(`The name was stored, but the email was not: ${emailError}. Retry to complete the pair.`);
      return;
    }
    setName('');
    setEmail('');
    toast.success('Git commit identity stored in the Vault');
    onSaved();
  }

  const input: React.CSSProperties = {
    width: '100%', boxSizing: 'border-box', padding: '8px 10px', borderRadius: 6,
    border: '1px solid #ccd3dc', fontSize: 13,
  };

  return (
    <div style={{ display: 'grid', gap: 11 }}>
      <p style={{ fontSize: 12.5, color: '#667', margin: 0, lineHeight: 1.45 }}>
        Aimee refuses to invent a commit author. This identity is sealed in the Vault and used by
        CLI, delegate, and workflow commits; it is never written to container environment config.
        Existing installations show one Setup item until both fields are stored. If you skip,
        reopen Setup from the persistent header chip to add them later.
      </p>
      <label style={{ fontSize: 12.5, fontWeight: 600 }}>
        Author name
        <input aria-label="Git author name" style={{ ...input, marginTop: 4 }} value={name}
          onChange={(event) => setName(event.target.value)} autoComplete="name" />
      </label>
      <label style={{ fontSize: 12.5, fontWeight: 600 }}>
        Author email
        <input aria-label="Git author email" type="email" style={{ ...input, marginTop: 4 }} value={email}
          onChange={(event) => setEmail(event.target.value)} autoComplete="email" />
      </label>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', gap: 10 }}>
        <button type="button" onClick={onSkip} disabled={saving}
          title="Commits will be refused until both fields are stored"
          style={{ border: 0, background: 'none', color: '#778', cursor: 'pointer', padding: 0 }}>
          Skip for now — commits will be refused
        </button>
        <Button variant="primary" disabled={saving} onClick={save}>
          {saving ? 'Saving…' : 'Save identity'}
        </Button>
      </div>
    </div>
  );
}
