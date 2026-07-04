// Minimal fetch helpers for the kb-console SPA. Every call is same-origin with
// credentials (the HttpOnly session cookie); mutating calls carry the per-session
// CSRF token the backend issued at login.

let csrfToken = '';

export function setCsrf(token: string) {
  csrfToken = token;
}

export async function apiGet<T = unknown>(path: string): Promise<T> {
  const r = await fetch(`/api${path}`, { credentials: 'same-origin' });
  if (!r.ok) throw new Error(`${r.status}`);
  return r.json() as Promise<T>;
}

export async function apiSend<T = unknown>(method: string, path: string, body?: unknown): Promise<T> {
  const r = await fetch(`/api${path}`, {
    method,
    credentials: 'same-origin',
    headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': csrfToken },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  if (!r.ok) throw new Error(`${r.status}`);
  return r.json() as Promise<T>;
}

export interface SessionInfo {
  csrf: string;
  break_glass: boolean;
}

// loadSession returns the current session (and stashes its CSRF token) or null.
export async function loadSession(): Promise<SessionInfo | null> {
  const r = await fetch('/api/session', { credentials: 'same-origin' });
  if (!r.ok) return null;
  const s = (await r.json()) as SessionInfo;
  setCsrf(s.csrf);
  return s;
}

// login exchanges an OIDC id_token (or a break-glass bearer) for a session.
export async function login(payload: { id_token?: string; break_glass_bearer?: string }): Promise<SessionInfo> {
  const r = await fetch('/api/login', {
    method: 'POST',
    credentials: 'same-origin',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(payload),
  });
  if (!r.ok) throw new Error(`login failed: ${r.status}`);
  const s = (await r.json()) as SessionInfo;
  setCsrf(s.csrf);
  return s;
}
