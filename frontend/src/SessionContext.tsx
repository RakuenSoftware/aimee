import { createContext, useCallback, useContext, useEffect, useMemo, useState } from 'react';
import type { ReactNode } from 'react';

/* A "session" is the app-level unit of work: one chat conversation bound to one
 * git project (plus the provider session ids that carry that conversation). It
 * is shown as a tab at the very top of the app; the vertical tool nav (Chat /
 * Editor / Workflows / …) all operate on the ACTIVE session's project. */
export interface Session {
  id: string;          // stable local id (also the React key)
  name: string;        // tab label
  projectRoot: string; // workspace root of the bound project ("" = none)
  projectName: string; // project (repo) name within the root ("" = none)
  claudeSid: string;   // claude provider session id (for --resume); reset by Clear
  aimeeSid: string;    // aimee session id (server-side per-session state)
  attachId: string;    // unified-presence attachment id (minted on first send)
}

interface SessionCtx {
  sessions: Session[];
  activeId: string;
  active: Session | null;
  addSession: (name?: string) => string;
  closeSession: (id: string) => void;
  selectSession: (id: string) => void;
  renameSession: (id: string, name: string) => void;
  /* Merge a partial update into one session (project binding, sids, attach id). */
  patchSession: (id: string, patch: Partial<Session>) => void;
}

const SESSIONS_KEY = 'aimee_sessions';
const ACTIVE_KEY = 'aimee_active_session';

let _idSeq = 0;
function newId(): string {
  // Unique without Math.random/Date — monotonic counter + a per-load base.
  _idSeq += 1;
  return `s${_idSeq}-${performance.now().toString(36).replace('.', '')}`;
}

function blankSession(name: string): Session {
  return { id: newId(), name, projectRoot: '', projectName: '', claudeSid: '', aimeeSid: '', attachId: '' };
}

function loadSessions(): Session[] {
  try {
    const raw = localStorage.getItem(SESSIONS_KEY);
    if (raw) {
      const parsed = JSON.parse(raw) as Partial<Session>[];
      if (Array.isArray(parsed) && parsed.length) {
        return parsed.map((s, i) => ({
          id: typeof s.id === 'string' && s.id ? s.id : newId(),
          name: typeof s.name === 'string' && s.name ? s.name : `Session ${i + 1}`,
          projectRoot: typeof s.projectRoot === 'string' ? s.projectRoot : '',
          projectName: typeof s.projectName === 'string' ? s.projectName : '',
          claudeSid: typeof s.claudeSid === 'string' ? s.claudeSid : '',
          aimeeSid: typeof s.aimeeSid === 'string' ? s.aimeeSid : '',
          attachId: typeof s.attachId === 'string' ? s.attachId : '',
        }));
      }
    }
  } catch { /* ignore */ }
  return [blankSession('Session 1')];
}

const Ctx = createContext<SessionCtx | null>(null);

export function SessionProvider({ children }: { children: ReactNode }) {
  const [sessions, setSessions] = useState<Session[]>(loadSessions);
  const [activeId, setActiveId] = useState<string>(() => {
    const stored = localStorage.getItem(ACTIVE_KEY);
    return stored || '';
  });

  // Keep an always-valid active id.
  useEffect(() => {
    if (!sessions.length) return;
    if (!sessions.some(s => s.id === activeId)) setActiveId(sessions[0].id);
  }, [sessions, activeId]);

  // Persist.
  useEffect(() => {
    try { localStorage.setItem(SESSIONS_KEY, JSON.stringify(sessions)); } catch { /* ignore */ }
  }, [sessions]);
  useEffect(() => {
    try { localStorage.setItem(ACTIVE_KEY, activeId); } catch { /* ignore */ }
  }, [activeId]);

  const addSession = useCallback((name?: string) => {
    const s = blankSession(name && name.trim() ? name.trim() : `Session ${Date.now() % 100000}`);
    setSessions(prev => [...prev, s]);
    setActiveId(s.id);
    return s.id;
  }, []);

  const closeSession = useCallback((id: string) => {
    setSessions(prev => {
      const next = prev.filter(s => s.id !== id);
      const result = next.length ? next : [blankSession('Session 1')];
      setActiveId(cur => (cur === id ? result[0].id : cur));
      return result;
    });
  }, []);

  const selectSession = useCallback((id: string) => setActiveId(id), []);

  const renameSession = useCallback((id: string, name: string) => {
    const n = name.trim();
    if (!n) return;
    setSessions(prev => prev.map(s => (s.id === id ? { ...s, name: n } : s)));
  }, []);

  const patchSession = useCallback((id: string, patch: Partial<Session>) => {
    setSessions(prev => prev.map(s => (s.id === id ? { ...s, ...patch } : s)));
  }, []);

  const active = useMemo(() => sessions.find(s => s.id === activeId) || sessions[0] || null, [sessions, activeId]);

  const value = useMemo<SessionCtx>(() => ({
    sessions, activeId: active?.id ?? '', active,
    addSession, closeSession, selectSession, renameSession, patchSession,
  }), [sessions, active, addSession, closeSession, selectSession, renameSession, patchSession]);

  return <Ctx.Provider value={value}>{children}</Ctx.Provider>;
}

export function useSessions(): SessionCtx {
  const ctx = useContext(Ctx);
  if (!ctx) throw new Error('useSessions must be used within a SessionProvider');
  return ctx;
}
