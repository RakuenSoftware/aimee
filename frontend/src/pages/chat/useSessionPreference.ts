import { useCallback, useEffect, useRef, useState } from 'react';

type Preference = { loaded: boolean; value: string; confirmed: string; revision: number; pending: number; error: string; writes: Promise<void> };

/** Keep server-owned preferences on their originating session. A late history
 * read cannot undo a user selection, and writes reach the server in click order. */
export function useSessionPreference(session: string, endpoint: string, field: string, enabled = true) {
  const values = useRef(new Map<string, Preference>());
  const [, refresh] = useState(0);
  const mounted = useRef(false);
  useEffect(() => { mounted.current = true; return () => { mounted.current = false; }; }, []);
  const redraw = useCallback(() => { if (mounted.current) refresh(value => value + 1); }, []);
  function entry(id: string): Preference {
    let value = values.current.get(id);
    if (!value) {
      value = { loaded: false, value: '', confirmed: '', revision: 0, pending: 0, error: '', writes: Promise.resolve() };
      values.current.set(id, value);
    }
    return value;
  }
  const current = entry(session);
  useEffect(() => {
    if (!enabled) return;
    const value = entry(session);
    let disposed = false;
    let abort: AbortController | undefined;
    const load = () => {
      if (value.pending || document.visibilityState === 'hidden') return;
      abort?.abort();
      const request = new AbortController();
      abort = request;
      const revision = value.revision;
      void fetch(`${endpoint}${session ? `?sid=${encodeURIComponent(session)}` : ''}`, { signal: request.signal })
        .then(response => { if (!response.ok) throw new Error('Could not load selection'); return response.json(); })
        .then((body: Record<string, unknown>) => {
          if (disposed || request.signal.aborted || value.revision !== revision || value.pending) return;
          if (!body || typeof body !== 'object' || typeof body[field] !== 'string') throw new Error('Invalid selection reply');
          value.value = value.confirmed = body[field];
          value.loaded = true;
          value.error = '';
          redraw();
        }).catch(() => {
          if (disposed || request.signal.aborted || value.revision !== revision || value.pending) return;
          value.error = value.loaded ? 'Could not refresh selection. Showing the last saved selection.' : 'Current selection is unavailable. Please try again.';
          redraw();
        });
    };
    load();
    window.addEventListener('focus', load);
    document.addEventListener('visibilitychange', load);
    return () => {
      disposed = true;
      abort?.abort();
      window.removeEventListener('focus', load);
      document.removeEventListener('visibilitychange', load);
    };
  }, [session, endpoint, field, enabled, redraw]);

  const change = useCallback((name: string) => {
    if (!enabled || !session || !name) return;
    const value = entry(session);
    const revision = ++value.revision;
    value.value = name;
    value.error = '';
    value.pending++;
    redraw();
    value.writes = value.writes.then(async () => {
      try {
        const response = await fetch(endpoint, {
          method: 'POST', headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': window._csrf || '' },
          body: JSON.stringify({ sid: session, [field]: name }),
        });
        if (!response.ok) throw new Error('Could not save selection');
        value.confirmed = name;
        value.loaded = true;
      } catch {
        if (value.revision === revision) {
          value.value = value.confirmed;
          value.error = 'Could not save selection. Please try again.';
        }
      } finally { value.pending--; redraw(); }
    });
  }, [session, endpoint, field, enabled, redraw]);
  return { known: enabled && (current.loaded || current.pending > 0), value: current.value, pending: current.pending > 0, error: current.error, change };
}
