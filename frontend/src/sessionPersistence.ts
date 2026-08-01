export interface SessionMessage {
  role: 'user' | 'assistant' | 'narration';
  text: string;
}

export interface SessionRecord {
  id: string;
  name: string;
  projectRoot: string;
  projectName: string;
  claudeSid: string;
  aimeeSid: string;
  attachId: string;
  messages: SessionMessage[];
}

export interface PersistedChatSession {
  id: string;
  title?: string;
  cwd?: string;
  provider_session_id?: string;
  messages?: SessionMessage[];
  created_at?: string;
  last_active?: string;
}

const cacheMessageLimit = 200;
const cacheMessageTextLimit = 32 * 1024;

// localStorage is only a fast/offline cache. Keep it bounded so an
// account-scoped server transcript cannot exhaust the browser quota and make
// every subsequent cache write fail silently.
export function sessionsForLocalCache(sessions: SessionRecord[]): SessionRecord[] {
  return sessions.map(session => ({
    ...session,
    messages: session.messages.slice(-cacheMessageLimit).map(message => ({
      ...message,
      text: message.text.slice(-cacheMessageTextLimit),
    })),
  }));
}

function validMessages(value: unknown): SessionMessage[] {
  if (!Array.isArray(value)) return [];
  return value.filter((message): message is SessionMessage => {
    if (!message || typeof message !== 'object') return false;
    const candidate = message as Partial<SessionMessage>;
    return (candidate.role === 'user' || candidate.role === 'assistant' || candidate.role === 'narration') &&
      typeof candidate.text === 'string';
  });
}

// Reconcile a server snapshot with a live browser buffer. A non-empty shorter
// snapshot can be a refresh racing an in-flight stream, but an explicit empty
// snapshot is authoritative (for example, history cleared on another device).
export function reconcileSessionMessages<T>(local: T[], server: T[]): T[] {
  return server.length === 0 || server.length >= local.length ? server : local;
}

function preferMessages(local: SessionMessage[], server: unknown): SessionMessage[] {
  if (!Array.isArray(server)) return local;
  const remote = validMessages(server);
  // Preserve the distinction between an absent transcript and an explicit
  // empty transcript before applying the live-refresh race guard above.
  return reconcileSessionMessages(local, remote);
}

function projectNameFromRoot(root: string): string {
  const trimmed = root.replace(/[\\/]+$/, '');
  const parts = trimmed.split(/[\\/]/);
  return parts[parts.length - 1] || '';
}

export function isPristineDefaultSession(session: SessionRecord): boolean {
  return session.messages.length === 0 && !session.projectRoot && !session.claudeSid &&
    (session.name === 'Session 1' || session.name === 'Chat');
}

// Browser storage from older builds carried no username. It is never a safe
// migration source on a shared browser: only a cache already stamped with the
// currently authenticated account may be uploaded or merged as legacy state.
export function cacheBelongsToAccount(username: string, cachedOwner: string): boolean {
  return !!username && !!cachedOwner && username === cachedOwner;
}

export function shouldOfferOwnerlessCacheImport(
  username: string,
  cachedOwner: string,
  sessions: SessionRecord[],
): boolean {
  return !!username && !cachedOwner && sessions.some(session => !isPristineDefaultSession(session));
}

// Reconcile the browser cache with the authenticated account's server state.
// After the one-time legacy migration, absence on the server means deletion on
// another browser and local-only rows are therefore dropped.
export function mergePersistedSessions(
  local: SessionRecord[],
  server: PersistedChatSession[],
  includeLegacyLocal: boolean,
): SessionRecord[] {
  const localByAimeeID = new Map(local.filter(s => s.aimeeSid).map(s => [s.aimeeSid, s]));
  const merged: SessionRecord[] = [];

  for (const remote of server) {
    if (!remote.id || merged.some(s => s.aimeeSid === remote.id)) continue;
    const cached = localByAimeeID.get(remote.id);
    const projectRoot = remote.cwd || cached?.projectRoot || '';
    merged.push({
      id: cached?.id || remote.id,
      name: remote.title?.trim() || cached?.name || 'Chat',
      projectRoot,
      projectName: remote.cwd
        ? projectNameFromRoot(remote.cwd)
        : cached?.projectName || projectNameFromRoot(projectRoot),
      claudeSid: remote.provider_session_id || cached?.claudeSid || '',
      aimeeSid: remote.id,
      // Attachments are live browser surfaces and must never cross devices.
      attachId: '',
      messages: preferMessages(cached?.messages || [], remote.messages),
    });
    localByAimeeID.delete(remote.id);
  }

  if (includeLegacyLocal) {
    for (const session of local) {
      if (!localByAimeeID.has(session.aimeeSid)) continue;
      if (server.length > 0 && isPristineDefaultSession(session)) continue;
      merged.push(session);
    }
  }

  return merged;
}

export function sessionsMissingFromServer(
  local: SessionRecord[],
  server: PersistedChatSession[],
): SessionRecord[] {
  const byID = new Map(server.map(session => [session.id, session]));
  return local.filter(session => {
    if (!session.aimeeSid || isPristineDefaultSession(session)) return false;
    const remote = byID.get(session.aimeeSid);
    if (!remote) return true;
    const remoteMessages = validMessages(remote.messages);
    return session.messages.length > 0 && remoteMessages.length === 0;
  });
}

// Older webchat builds accidentally stored the provider-emitted thread id as
// the session's primary key. A legacy local tab is the only place where both ids
// still coexist, so identify those aliases during the one-time migration.
export function legacyProviderAliasIDs(
  local: SessionRecord[],
  server: PersistedChatSession[],
): string[] {
  const providerIDs = new Set(local.map(session => session.claudeSid).filter(Boolean));
  const stableIDs = new Set(local.map(session => session.aimeeSid).filter(Boolean));
  return server
    .map(session => session.id)
    .filter(id => id && providerIDs.has(id) && !stableIDs.has(id));
}
