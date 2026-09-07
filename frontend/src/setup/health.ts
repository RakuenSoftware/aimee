/* Dependency health for the "aimee is not fully functional" banner.
 *
 * Kept separate from readiness.ts: that module answers "has the operator
 * finished SETUP", this one answers "is the running instance HEALTHY". They
 * fail at different times and say different things — a fully configured
 * instance whose knowledge service just died is ready and unhealthy at once.
 *
 * The decision is a pure function so the wording and the trigger can be tested
 * without a server, and so a future dependency only has to be added in one
 * place. */

export type DepState = 'ok' | 'fail' | 'unknown' | string;

export interface HealthSnapshot {
  ready?: boolean;
  status?: string;
  dependencies?: Record<string, DepState>;
}

export interface HealthBanner {
  /** Short line for the banner itself. */
  title: string;
  /** What the user should expect to be broken, in their terms. */
  detail: string;
}

/* Only an outright "fail" is worth a banner. "unknown" means the server's
 * readiness sampler has not run yet, which is the normal state for the first
 * few seconds after a restart — showing a scary banner then would train users
 * to dismiss it, and the one time it matters they would. */
export function healthBanner(snap: HealthSnapshot | null | undefined): HealthBanner | null {
  if (!snap) return null;
  const deps = snap.dependencies || {};
  const kbDown = deps.kb === 'fail';
  const retrievalDown = deps.retrieval === 'fail';
  const db1Down = deps.db1 === 'fail';

  if (!kbDown && !retrievalDown && !db1Down) return null;

  if (db1Down) {
    return {
      title: 'aimee is not fully functional — the local database is unreachable',
      detail: 'Personal memory, sessions, and history may be unavailable while this lasts.',
    };
  }
  if (kbDown) {
    return {
      title: 'aimee is not fully functional — the knowledge service is unreachable',
      detail:
        'Shared knowledge and KB search are unavailable while this lasts. Personal memory remains ' +
        'local and available. An empty KB result does not mean the content is missing.',
    };
  }
  return {
    title: 'aimee is not fully functional — retrieval is unavailable',
    detail:
      'Search returns no results while this lasts. An empty result does not mean the content is ' +
      'missing.',
  };
}
