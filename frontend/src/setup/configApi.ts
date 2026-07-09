/* Thin, testable wrappers over the config endpoints the Settings page already
 * uses. Extracted so the wizard's write path (and its error handling) can be unit
 * tested with a stubbed fetch — vitest runs in node, so there is no real network
 * or DOM. `fetchImpl` is injectable for exactly that reason. */

export type ConfigMap = Record<string, unknown>;

type FetchLike = typeof fetch;

function csrf(): string {
  try {
    if (typeof window !== 'undefined') return (window as { _csrf?: string })._csrf || '';
  } catch {
    /* ignore */
  }
  return '';
}

/** Load the current config map (GET /api/config → { config }). Returns {} on any
 * failure so callers degrade rather than throw. */
export async function loadConfig(opts?: { fetchImpl?: FetchLike }): Promise<ConfigMap> {
  const f = opts?.fetchImpl ?? fetch;
  try {
    const r = await f('/api/config', { headers: { 'X-CSRF-Token': csrf() } });
    const d = (await r.json()) as { config?: ConfigMap };
    return d.config ?? {};
  } catch {
    return {};
  }
}

export interface SaveResult {
  ok: boolean;
  error?: string;
  /** The persisted value the server echoed back (falls back to the input). */
  value?: unknown;
}

/** Persist one config value (POST /api/config/set { key, value }). Mirrors the
 * Settings page's success test (2xx && !error) and never throws — a network or
 * parse failure returns { ok: false, error }. */
export async function saveConfigValue(
  key: string,
  value: unknown,
  opts?: { fetchImpl?: FetchLike },
): Promise<SaveResult> {
  const f = opts?.fetchImpl ?? fetch;
  try {
    const r = await f('/api/config/set', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': csrf() },
      body: JSON.stringify({ key, value }),
    });
    let data: { error?: string; value?: unknown } = {};
    try {
      data = (await r.json()) as { error?: string; value?: unknown };
    } catch {
      /* empty/invalid body — fall through to status check */
    }
    if (r.status >= 200 && r.status < 300 && !data.error) {
      return { ok: true, value: data.value !== undefined ? data.value : value };
    }
    return { ok: false, error: data.error || `save failed (${r.status})` };
  } catch (e) {
    return { ok: false, error: e instanceof Error ? e.message : 'network error' };
  }
}
