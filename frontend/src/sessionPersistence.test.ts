import { describe, expect, it } from 'vitest';
import {
  cacheBelongsToAccount,
  legacyProviderAliasIDs,
  mergePersistedSessions,
  reconcileSessionMessages,
  sessionsForLocalCache,
  sessionsMissingFromServer,
  shouldOfferOwnerlessCacheImport,
  type SessionRecord,
} from './sessionPersistence';

function local(overrides: Partial<SessionRecord> = {}): SessionRecord {
  return {
    id: 'ui-1',
    name: 'Local chat',
    projectRoot: '/work/project',
    projectName: 'project',
    claudeSid: 'provider-old',
    aimeeSid: 'web-stable',
    attachId: 'browser-only-attachment',
    messages: [{ role: 'user', text: 'hello' }],
    ...overrides,
  };
}

describe('account-scoped session persistence', () => {
  it('never trusts an ownerless or differently-owned browser cache', () => {
    expect(cacheBelongsToAccount('alice', '')).toBe(false);
    expect(cacheBelongsToAccount('alice', 'bob')).toBe(false);
    expect(cacheBelongsToAccount('', 'alice')).toBe(false);
    expect(cacheBelongsToAccount('alice', 'alice')).toBe(true);
  });

  it('offers explicit ownerless import only for real legacy state', () => {
    const pristine = local({
      name: 'Session 1', projectRoot: '', projectName: '', claudeSid: '', messages: [],
    });
    expect(shouldOfferOwnerlessCacheImport('alice', '', [local()])).toBe(true);
    expect(shouldOfferOwnerlessCacheImport('alice', '', [pristine])).toBe(false);
    expect(shouldOfferOwnerlessCacheImport('alice', 'bob', [local()])).toBe(false);
    expect(shouldOfferOwnerlessCacheImport('', '', [local()])).toBe(false);
  });

  it('hydrates a fresh browser with transcript and provider resume id', () => {
    const got = mergePersistedSessions([], [{
      id: 'web-stable',
      title: 'Server chat',
      cwd: '/work/project',
      provider_session_id: 'provider-new',
      messages: [
        { role: 'user', text: 'hello' },
        { role: 'assistant', text: 'hi back' },
      ],
    }], false);

    expect(got).toEqual([{
      id: 'web-stable',
      name: 'Server chat',
      projectRoot: '/work/project',
      projectName: 'project',
      claudeSid: 'provider-new',
      aimeeSid: 'web-stable',
      attachId: '',
      messages: [
        { role: 'user', text: 'hello' },
        { role: 'assistant', text: 'hi back' },
      ],
    }]);
  });

  it('uses an explicit empty server transcript to clear stale browser history', () => {
    const [got] = mergePersistedSessions([local()], [{
      id: 'web-stable',
      cwd: '/work/project',
      messages: [],
    }], false);

    expect(got.messages).toEqual([]);
    expect(reconcileSessionMessages(local().messages, [])).toEqual([]);
  });

  it('keeps a longer live transcript during a non-empty stale server refresh', () => {
    const live = [
      { role: 'user' as const, text: 'hello' },
      { role: 'assistant' as const, text: 'still streaming' },
    ];
    expect(reconcileSessionMessages(live, [live[0]])).toBe(live);
  });

  it('preserves the local UI key while refreshing stable server state', () => {
    const got = mergePersistedSessions([local()], [{
      id: 'web-stable',
      title: 'Renamed elsewhere',
      provider_session_id: 'provider-new',
      messages: [
        { role: 'user', text: 'hello' },
        { role: 'assistant', text: 'hi back' },
      ],
    }], false);

    expect(got[0].id).toBe('ui-1');
    expect(got[0].name).toBe('Renamed elsewhere');
    expect(got[0].claudeSid).toBe('provider-new');
    expect(got[0].attachId).toBe('');
    expect(got[0].messages).toHaveLength(2);
  });

  it('drops local sessions deleted on another browser after migration', () => {
    expect(mergePersistedSessions([local()], [], false)).toEqual([]);
  });

  it('identifies legacy browser-only chats for one-time upload', () => {
    const missing = sessionsMissingFromServer([local()], []);
    expect(missing.map(session => session.aimeeSid)).toEqual(['web-stable']);
  });

  it('seeds an existing metadata-only server row from the legacy browser transcript', () => {
    const missing = sessionsMissingFromServer([local()], [{ id: 'web-stable', messages: [] }]);
    expect(missing.map(session => session.aimeeSid)).toEqual(['web-stable']);
  });

  it('does not upload a pristine default tab during first-device restore', () => {
    const pristine = local({
      name: 'Session 1', projectRoot: '', projectName: '', claudeSid: '', messages: [],
    });
    expect(sessionsMissingFromServer([pristine], [])).toEqual([]);
  });

  it('drops a pristine placeholder alongside real legacy sessions', () => {
    const pristine = local({
      id: 'placeholder', aimeeSid: 'placeholder', name: 'Session 1', projectRoot: '',
      projectName: '', claudeSid: '', messages: [],
    });
    expect(mergePersistedSessions([pristine, local()], [{ id: 'server-chat' }], true)
      .map(session => session.aimeeSid)).toEqual(['server-chat', 'web-stable']);
  });

  it('bounds transcripts written to the browser cache', () => {
    const messages = Array.from({ length: 250 }, (_, i) => ({
      role: 'user' as const,
      text: `${i}:` + 'x'.repeat(40_000),
    }));
    const [cached] = sessionsForLocalCache([local({ messages })]);
    expect(cached.messages).toHaveLength(200);
    expect(cached.messages[0].text.length).toBe(32 * 1024);
    expect(cached.messages.at(-1)?.text.length).toBe(32 * 1024);
  });

  it('recognizes the provider-id rows written by older webchat builds', () => {
    expect(legacyProviderAliasIDs([local()], [
      { id: 'provider-old', title: 'legacy wrong-key row' },
      { id: 'another-session' },
    ])).toEqual(['provider-old']);
  });
});
