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
export async function fetchSetupAccountReady(): Promise<boolean> {
  const { ok, data } = await getJSON('/api/setup/account');
  return ok && data.complete === true;
}

export async function fetchProjectCount(): Promise<number> {
  const { ok, data } = await getJSON('/api/git/projects');
  return ok && Array.isArray(data.projects) ? data.projects.length : 0;
}

export async function fetchHostCount(): Promise<number> {
  const { ok, data } = await getJSON('/api/git/credentials');
  return ok && Array.isArray(data.hosts) ? data.hosts.length : 0;
}

export async function fetchAppliance(): Promise<boolean> {
  const { ok, data } = await getJSON('/api/setup/appliance');
  return ok && data.appliance === true;
}
