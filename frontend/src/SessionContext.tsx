import { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState } from 'react';
import type { ReactNode } from 'react';
import {
  cacheBelongsToAccount,
  legacyProviderAliasIDs,
  mergePersistedSessions,
  sessionsForLocalCache,
  sessionsMissingFromServer,
  shouldOfferOwnerlessCacheImport,
  type PersistedChatSession,
  type SessionMessage,
  type SessionRecord,
} from './sessionPersistence';

/* A "session" is the app-level unit of work: one chat conversation bound to one
 * git project (plus the provider session ids that carry that conversation). The
 * authenticated server account is authoritative; localStorage is only a fast
 * cache and a one-time migration source for pre-server transcripts. */
export interface Session extends SessionRecord {
  messages: SessionMessage[];
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
const SERVER_SYNC_KEY = 'aimee_server_sessions_authoritative_v1';
const CACHE_OWNER_KEY = 'aimee_sessions_owner';
const LEGACY_TABS_KEY = 'aimee_chat_tabs';

function newAimeeSessionID(): string {
  try {
    if (globalThis.crypto?.randomUUID) return `web-${globalThis.crypto.randomUUID()}`;
  } catch { /* ignore */ }
  return `web-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 10)}`;
}

function blankSession(name: string): Session {
  const id = newAimeeSessionID();
  return {
    id,
    name,
    projectRoot: '',
    projectName: '',
    claudeSid: '',
    aimeeSid: id,
    attachId: '',
    messages: [],
  };
}

interface LegacyTab {
  sessionId?: string;
  aimeeSid?: string;
  sid?: string;
  messages?: SessionMessage[];
}

function loadLegacyTabs(): Map<string, LegacyTab> {
  const out = new Map<string, LegacyTab>();
  try {
    const parsed = JSON.parse(localStorage.getItem(LEGACY_TABS_KEY) || '[]') as LegacyTab[];
    if (Array.isArray(parsed)) {
      for (const tab of parsed) if (tab?.sessionId) out.set(tab.sessionId, tab);
    }
  } catch { /* ignore */ }
  return out;
}

function normalizeMessages(value: unknown): SessionMessage[] {
  if (!Array.isArray(value)) return [];
  return value.filter((message): message is SessionMessage => {
    if (!message || typeof message !== 'object') return false;
    const candidate = message as Partial<SessionMessage>;
    return (candidate.role === 'user' || candidate.role === 'assistant' || candidate.role === 'narration') &&
      typeof candidate.text === 'string';
  });
}

function loadSessions(): Session[] {
  try {
    const raw = localStorage.getItem(SESSIONS_KEY);
    if (raw) {
      const parsed = JSON.parse(raw) as Partial<Session>[];
      if (Array.isArray(parsed) && parsed.length) {
        const legacyTabs = loadLegacyTabs();
        return parsed.map((session, i) => {
          const legacy = typeof session.id === 'string' ? legacyTabs.get(session.id) : undefined;
          const aimeeSid = (typeof session.aimeeSid === 'string' && session.aimeeSid) ||
            legacy?.aimeeSid || newAimeeSessionID();
          return {
            id: typeof session.id === 'string' && session.id ? session.id : aimeeSid,
            name: typeof session.name === 'string' && session.name ? session.name : `Session ${i + 1}`,
            projectRoot: typeof session.projectRoot === 'string' ? session.projectRoot : '',
            projectName: typeof session.projectName === 'string' ? session.projectName : '',
            claudeSid: (typeof session.claudeSid === 'string' && session.claudeSid) || legacy?.sid || '',
            aimeeSid,
            // Presence attachments are browser-process-local and cannot be resumed.
            attachId: '',
            messages: normalizeMessages(session.messages).length
              ? normalizeMessages(session.messages)
              : normalizeMessages(legacy?.messages),
          };
        });
      }
    }
  } catch { /* ignore */ }
  return [blankSession('Session 1')];
}

async function persistSession(session: Session, legacyProviderAliasID = ''): Promise<boolean> {
  try {
    const response = await fetch('/api/chat/session', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': window._csrf || '' },
      body: JSON.stringify({
        id: session.aimeeSid,
        title: session.name,
        cwd: session.projectRoot,
        legacy_provider_alias_id: legacyProviderAliasID || undefined,
        // The backend imports this only when its transcript is still empty.
        // Provider ids are opaque resume credentials and are never imported
        // directly from browser storage. During the one-time legacy upgrade the
        // optional alias only asks the backend to transfer a binding already
        // present in this authenticated user's server-owned legacy row.
        messages: session.messages,
      }),
    });
    return response.ok;
  } catch {
    return false; // offline: the local cache remains available
  }
}

async function forgetSessionRequest(aimeeSid: string): Promise<boolean> {
  if (!aimeeSid) return true;
  try {
    const response = await fetch(`/api/chat/session?sid=${encodeURIComponent(aimeeSid)}`, {
      method: 'DELETE',
      headers: { 'X-CSRF-Token': window._csrf || '' },
    });
    return response.ok;
  } catch {
    return false;
  }
}

function forgetSession(aimeeSid: string): void {
  void forgetSessionRequest(aimeeSid);
}

const Ctx = createContext<SessionCtx | null>(null);

export function SessionProvider({ children }: { children: ReactNode }) {
  const [sessions, setSessions] = useState<Session[]>(loadSessions);
  const sessionsRef = useRef<Session[]>(sessions);
  const [restored, setRestored] = useState(false);
  const [activeId, setActiveId] = useState<string>(() => {
    try { return localStorage.getItem(ACTIVE_KEY) || ''; } catch { return ''; }
  });

  useEffect(() => { sessionsRef.current = sessions; }, [sessions]);

  // Keep an always-valid active id.
  useEffect(() => {
    if (!sessions.length) return;
    if (!sessions.some(session => session.id === activeId)) setActiveId(sessions[0].id);
  }, [sessions, activeId]);

  // Persist the fast browser cache. The account-scoped server remains the source
  // of truth after bootstrap succeeds.
  useEffect(() => {
    try { localStorage.setItem(SESSIONS_KEY, JSON.stringify(sessionsForLocalCache(sessions))); } catch { /* ignore */ }
  }, [sessions]);
  useEffect(() => {
    try { localStorage.setItem(ACTIVE_KEY, activeId); } catch { /* ignore */ }
  }, [activeId]);

  const refreshFromServer = useCallback(async () => {
    const controller = new AbortController();
    const timeout = window.setTimeout(() => controller.abort(), 10_000);
    let cacheVerified = false;
    const replaceUnverifiedCache = () => {
      if (cacheVerified) return;
      const next = [blankSession('Session 1')];
      sessionsRef.current = next;
      setSessions(next);
    };
    try {
      // allSettled lets a successful identity response validate an owned cache
      // even when the sessions request is temporarily offline. A rejected
      // identity request can never authorize data loaded from browser storage.
      const [identityResult, sessionsResult] = await Promise.allSettled([
        fetch('/api/auth/me', { signal: controller.signal }),
        fetch('/api/chat/sessions', { signal: controller.signal }),
      ]);
      if (identityResult.status !== 'fulfilled' || !identityResult.value.ok) {
        replaceUnverifiedCache();
        return;
      }
      const identity = await identityResult.value.json() as { username?: string };
      const username = identity.username?.trim() || '';

      let cachedOwner = '';
      try { cachedOwner = localStorage.getItem(CACHE_OWNER_KEY) || ''; } catch { /* ignore */ }
      let trustedCache = cacheBelongsToAccount(username, cachedOwner);
      cacheVerified = trustedCache;
      if (sessionsResult.status !== 'fulfilled') {
        replaceUnverifiedCache();
        return;
      }
      const response = sessionsResult.value;
      if (!trustedCache && !response.ok) {
        // Never render another account's cache just because session restore is
        // temporarily unavailable. A fresh placeholder is safe and stays local.
        replaceUnverifiedCache();
      }
      if (!response.ok) return;
      const body = await response.json() as unknown;
      if (!Array.isArray(body)) return;
      const remote = body as PersistedChatSession[];

      // Pre-account browser caches have no trustworthy owner. Never attach one
      // silently to the first person who logs in on a shared browser. Offer a
      // one-time, explicit binding only when there is real legacy state; a
      // mismatched stamped owner is never eligible for this path.
      if (shouldOfferOwnerlessCacheImport(username, cachedOwner, sessionsRef.current)) {
        const confirmed = window.confirm(
          `Import chats stored in this browser into the account “${username}”? ` +
          'Only continue if these chats belong to you.',
        );
        if (confirmed) {
          cachedOwner = username;
          trustedCache = true;
          cacheVerified = true;
          try { localStorage.setItem(CACHE_OWNER_KEY, username); } catch { /* ignore */ }
        }
      }
      let alreadyAuthoritative = false;
      try {
        const marker = localStorage.getItem(SERVER_SYNC_KEY) || '';
        alreadyAuthoritative = !trustedCache ||
          (username ? marker === username : marker === '1');
      } catch { /* ignore */ }

      // A cache owned by a different authenticated account is never a migration
      // source. Only the server's account-scoped rows participate in this merge.
      const local = trustedCache ? sessionsRef.current : [];
      const aliasIDs = alreadyAuthoritative ? [] : legacyProviderAliasIDs(local, remote);
      const aliases = new Set(aliasIDs);
      const canonicalRemote = remote.filter(session => !aliases.has(session.id));
      const legacy = alreadyAuthoritative ? [] : sessionsMissingFromServer(local, canonicalRemote);
      const merged = mergePersistedSessions(local, canonicalRemote, !alreadyAuthoritative);
      const next = merged.length ? merged : [blankSession('Session 1')];
      sessionsRef.current = next;
      setSessions(next);

      // One-time upgrade path: seed server rows/transcripts that only existed in
      // localStorage before account-scoped persistence became authoritative.
      if (!alreadyAuthoritative) {
        const uploaded = await Promise.all(legacy.map(session => persistSession(
          session,
          aliases.has(session.claudeSid) ? session.claudeSid : '',
        )));
        const deleted = uploaded.every(Boolean)
          ? await Promise.all(aliasIDs.map(forgetSessionRequest))
          : [false];
        if (uploaded.every(Boolean) && deleted.every(Boolean)) {
          try {
            localStorage.setItem(SERVER_SYNC_KEY, username || '1');
            if (username) localStorage.setItem(CACHE_OWNER_KEY, username);
          } catch { /* ignore */ }
        }
        return;
      }
      try {
        localStorage.setItem(SERVER_SYNC_KEY, username || '1');
        if (username) localStorage.setItem(CACHE_OWNER_KEY, username);
      } catch { /* ignore */ }
    } catch {
      // Parsing failures and aborts are equivalent to an unavailable identity:
      // never expose an unverified cache after the loading gate is released.
      replaceUnverifiedCache();
    }
    finally {
      window.clearTimeout(timeout);
      setRestored(true);
    }
  }, []);

  useEffect(() => {
    void refreshFromServer();
  }, [refreshFromServer]);

  // Pull in sessions created, renamed, or closed on another device during a
  // long-lived browser session. The server remains authoritative; focus and a
  // transition back to a visible tab are cheap reconciliation points.
  useEffect(() => {
    const refreshWhenVisible = () => {
      if (document.visibilityState === 'visible') void refreshFromServer();
    };
    window.addEventListener('focus', refreshWhenVisible);
    document.addEventListener('visibilitychange', refreshWhenVisible);
    return () => {
      window.removeEventListener('focus', refreshWhenVisible);
      document.removeEventListener('visibilitychange', refreshWhenVisible);
    };
  }, [refreshFromServer]);

  const addSession = useCallback((name?: string) => {
    const session = blankSession(name && name.trim() ? name.trim() : `Session ${Date.now() % 100000}`);
    setSessions(previous => [...previous, session]);
    setActiveId(session.id);
    void persistSession(session);
    return session.id;
  }, []);

  const closeSession = useCallback((id: string) => {
    const target = sessionsRef.current.find(session => session.id === id);
    if (target) forgetSession(target.aimeeSid);
    setSessions(previous => {
      const remaining = previous.filter(session => session.id !== id);
      const next = remaining.length ? remaining : [blankSession('Session 1')];
      setActiveId(current => current === id ? next[0].id : current);
      return next;
    });
  }, []);

  const selectSession = useCallback((id: string) => setActiveId(id), []);

  const renameSession = useCallback((id: string, name: string) => {
    const normalized = name.trim();
    if (!normalized) return;
    setSessions(previous => previous.map(session => {
      if (session.id !== id) return session;
      const next = { ...session, name: normalized };
      void persistSession(next);
      return next;
    }));
  }, []);

  const patchSession = useCallback((id: string, patch: Partial<Session>) => {
    setSessions(previous => previous.map(session => {
      if (session.id !== id) return session;
      const next = { ...session, ...patch };
      if (next.aimeeSid !== session.aimeeSid) forgetSession(session.aimeeSid);
      void persistSession(next);
      return next;
    }));
  }, []);

  const active = useMemo(
    () => sessions.find(session => session.id === activeId) || sessions[0] || null,
    [sessions, activeId],
  );

  const value = useMemo<SessionCtx>(() => ({
    sessions,
    activeId: active?.id ?? '',
    active,
    addSession,
    closeSession,
    selectSession,
    renameSession,
    patchSession,
  }), [sessions, active, addSession, closeSession, selectSession, renameSession, patchSession]);

  return (
    <Ctx.Provider value={value}>
      {restored ? children : (
        <div style={{ padding: 24, fontFamily: 'system-ui', color: 'var(--sg-text-faint)' }}>Loading chats…</div>
      )}
    </Ctx.Provider>
  );
}

export function useSessions(): SessionCtx {
  const ctx = useContext(Ctx);
  if (!ctx) throw new Error('useSessions must be used within a SessionProvider');
  return ctx;
}
