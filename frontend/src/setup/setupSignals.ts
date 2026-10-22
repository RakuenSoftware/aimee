import { resolveSetupDismissalScope } from './setupState';
function csrf(): string {
  try {
    if (typeof window !== 'undefined') return (window as { _csrf?: string })._csrf || '';
  } catch {
    /* ignore */
  }
  return '';
}

async function getJSON(path: string): Promise<{ ok: boolean; data: Record<string, unknown> }> {
  try {
    const response = await fetch(path, { headers: { 'X-CSRF-Token': csrf() } });
    const data = await response.json().catch(() => ({}));
    return { ok: response.ok, data };
  } catch {
    return { ok: false, data: {} };
  }
}

/** Runtime signals that are not part of GET /api/config but still drive setup. */
export type SetupAccountStatus = { complete: boolean; dismissal_scope?: string };
export async function fetchSetupAccountStatus(): Promise<SetupAccountStatus> {
  const { ok, data } = await getJSON('/api/setup/account');
  const scope = ok && typeof data.dismissal_scope === 'string' && /^[a-f0-9]{64}$/.test(data.dismissal_scope)
    ? data.dismissal_scope : undefined;
  resolveSetupDismissalScope(scope);
  return { complete: ok && data.complete === true, dismissal_scope: scope };
}
export async function fetchSetupAccountReady(): Promise<boolean> {
  return (await fetchSetupAccountStatus()).complete;
}

export async function fetchProjectCount(): Promise<number> {
  const { ok, data } = await getJSON('/api/git/projects');
  return ok && Array.isArray(data.projects) ? data.projects.length : 0;
}

export async function fetchHostCount(): Promise<number> {
  const { ok, data } = await getJSON('/api/git/credentials');
  return ok && Array.isArray(data.hosts) ? data.hosts.length : 0;
}

export async function fetchGitIdentityReady(): Promise<boolean> {
  const { ok, data } = await getJSON('/api/vault/credentials');
  if (!ok || !Array.isArray(data.credentials)) return false;
  const names = new Set(
    data.credentials
      .filter((entry): entry is { agent: string; cred: string } =>
        typeof entry === 'object' && entry !== null &&
        (entry as { agent?: unknown }).agent === 'git' &&
        typeof (entry as { cred?: unknown }).cred === 'string')
      .map((entry) => entry.cred),
  );
  return names.has('author_name') && names.has('author_email');
}

export async function fetchAppliance(): Promise<boolean> {
  const { ok, data } = await getJSON('/api/setup/appliance');
  return ok && data.appliance === true;
}
