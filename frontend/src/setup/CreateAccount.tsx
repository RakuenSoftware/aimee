import { useState } from 'react';
import { Button } from '@rakuensoftware/smoothgui';

function csrf(): string {
  try {
    return (window as { _csrf?: string })._csrf || '';
  } catch {
    return '';
  }
}

/** Mirror of setupAccountMinPassword (runtime-web/setup_account.go). */
const MIN_PASSWORD = 6;

export default function CreateAccount({ onCreated }: { onCreated: (username: string) => void }) {
  const [username, setUsername] = useState('');
  const [password, setPassword] = useState('');
  const [confirmation, setConfirmation] = useState('');
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState('');

  async function create() {
    const user = username.trim();
    if (!/^[a-z][a-z0-9_-]{0,31}$/.test(user)) {
      setError('Use 1–32 lowercase letters, digits, hyphens, or underscores; start with a letter.');
      return;
    }
    if (user === 'aimee') {
      setError('Choose a username other than aimee.');
      return;
    }
    // Keep in step with setupAccountMinPassword in runtime-web/setup_account.go:
    // the server rejects independently, and a client limit that is stricter than
    // the server's just refuses a password the deployment would have accepted.
    if (password.length < MIN_PASSWORD) {
      setError(`Password must be at least ${MIN_PASSWORD} characters.`);
      return;
    }
    if (password !== confirmation) {
      setError('Passwords do not match.');
      return;
    }

    setSaving(true);
    setError('');
    try {
      const response = await fetch('/api/setup/account', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': csrf() },
        body: JSON.stringify({
          username: user,
          password,
          password_confirmation: confirmation,
        }),
      });
      const data = await response.json().catch(() => ({}));
      if (!response.ok) {
        setError(data.error || `Could not create account (${response.status}).`);
        return;
      }
      setPassword('');
      setConfirmation('');
      onCreated(user);
    } catch {
      setError('aimee-server unavailable');
    } finally {
      setSaving(false);
    }
  }

  return (
    <div style={{ display: 'grid', gap: 12, marginBottom: 8 }}>
      <div style={{ fontSize: 12.5, color: 'var(--sg-text-muted)', lineHeight: 1.5 }}>
        Replace the temporary bootstrap login before configuring this instance. Its plaintext is
        removed after this step; the new login is stored on the persistent volume and survives image updates.
      </div>
      <label>
        <div style={labelStyle}>Username</div>
        <input style={inputStyle} autoComplete="username" value={username}
          onChange={(event) => setUsername(event.target.value)} placeholder="your username" />
      </label>
      <label>
        <div style={labelStyle}>Password</div>
        <input style={inputStyle} type="password" autoComplete="new-password" value={password}
          onChange={(event) => setPassword(event.target.value)} />
      </label>
      <label>
        <div style={labelStyle}>Confirm password</div>
        <input style={inputStyle} type="password" autoComplete="new-password" value={confirmation}
          onChange={(event) => setConfirmation(event.target.value)}
          onKeyDown={(event) => { if (event.key === 'Enter' && !saving) create(); }} />
      </label>
      {error && <div style={{ color: 'var(--sg-danger-dark)', fontSize: 12.5 }}>{error}</div>}
      <div>
        <Button variant="primary" disabled={saving || !username.trim() || !password || !confirmation}
          onClick={create}>
          {saving ? 'Creating…' : 'Create account & continue'}
        </Button>
      </div>
    </div>
  );
}

const labelStyle: React.CSSProperties = {
  fontSize: 12.5, fontWeight: 600, marginBottom: 3,
};

const inputStyle: React.CSSProperties = {
  width: '100%', boxSizing: 'border-box', padding: '7px 9px', borderRadius: 6,
  border: '1px solid var(--sg-border-medium)', fontSize: 13, fontFamily: 'ui-monospace, monospace',
};
