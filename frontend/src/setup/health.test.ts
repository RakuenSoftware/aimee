import { describe, it, expect } from 'vitest';
import { healthBanner } from './health';

describe('healthBanner', () => {
  it('says nothing when every dependency is ok', () => {
    expect(healthBanner({ ready: true, status: 'ok', dependencies: { kb: 'ok', db1: 'ok', retrieval: 'ok' } })).toBeNull();
  });

  it('does not cry wolf while readiness is still unknown', () => {
    // The normal state for the first seconds after a restart. A banner here
    // would train users to dismiss it, and the one time it matters they would.
    expect(healthBanner({ status: 'unknown', dependencies: { kb: 'unknown', db1: 'unknown', retrieval: 'unknown' } })).toBeNull();
    expect(healthBanner({ dependencies: {} })).toBeNull();
    expect(healthBanner(null)).toBeNull();
    expect(healthBanner(undefined)).toBeNull();
  });

  it('reports a kb outage and tells the user what it means', () => {
    const b = healthBanner({ dependencies: { kb: 'fail', db1: 'ok', retrieval: 'ok' } });
    expect(b).not.toBeNull();
    expect(b!.title).toContain('not fully functional');
    expect(b!.title).toContain('knowledge service');
    // The misreading this exists to prevent.
    expect(b!.detail).toContain('does not mean');
    // And the reassurance that stops the user re-cloning by hand.
    expect(b!.detail).toContain('indexed automatically');
  });

  it('reports retrieval separately from the kb', () => {
    const b = healthBanner({ dependencies: { kb: 'ok', db1: 'ok', retrieval: 'fail' } });
    expect(b!.title).toContain('retrieval is unavailable');
  });

  it('names the kb when several things are down at once', () => {
    // retrieval failing is usually a symptom of the kb being down; two alarms
    // for one cause is noise.
    const b = healthBanner({ dependencies: { kb: 'fail', db1: 'fail', retrieval: 'fail' } });
    expect(b!.title).toContain('knowledge service');
  });

  it('reports a database outage when only it is down', () => {
    const b = healthBanner({ dependencies: { kb: 'ok', db1: 'fail', retrieval: 'ok' } });
    expect(b!.title).toContain('local database');
  });
});
